#include "RenderSelfTest.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "audio/Synth.h"
#include "daw/Appearance.h"
#include "daw/Arrangement.h"
#include "daw/DeviceControls.h"
#include "daw/PianoRoll.h"
#include "daw/Project.h"
#include "daw/ProjectStore.h"
#include "daw/StepSequencer.h"
#include "daw/SynthView.h"
#include "engine/Renderer.h"
#include "engine/Window.h"
#include "ui/Context.h"
#include "ui/DrawList.h"
#include "ui/FrameStats.h"
#include "ui/Image.h"
#include "ui/Prim.h"
#include "ui/Theme.h"
#include "ui/UI.h"

// stb's PNG writer, static so its copy stays apart from the browser self-test's
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STB_IMAGE_WRITE_STATIC
#include <stb_image_write.h>

// stb's PNG reader: declarations only, the implementation is compiled in src/daw/Archive.cpp
#define STBI_NO_STDIO
#include <stb_image.h>

namespace tests {

namespace {

namespace fs = std::filesystem;

const int kTolerance = 2;            // per channel
const double kMaxBeyondShare = 0.0005; // share of pixels allowed beyond the tolerance
const int kSettleFrames = 300;       // at most this many frames before drawing
const int kSteadyFrames = 3;         // the same fingerprint this many frames running

// A picture: opaque RGB, 3 bytes a pixel, rows packed, top row first
struct Picture {
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<uint8_t> rgb;
    ui::FrameStats stats;
};

ui::Rect box(float x, float y, float w, float h)
{
    return {x, y, w, h};
}

// ---- PNG io ----

fs::path goldenPath(const std::string &scene)
{
    return fs::path(std::string(VIBIN_TEST_DIR) + "/render/" + scene + ".png");
}

fs::path outputFolder()
{
    return fs::temp_directory_path() / "vibin-render";
}

bool writePng(const fs::path &path, const Picture &picture, std::string &error)
{
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    const std::string name = path.string();
    const int stride = static_cast<int>(picture.width * 3);
    if (!stbi_write_png(name.c_str(), static_cast<int>(picture.width), static_cast<int>(picture.height), 3,
                        picture.rgb.data(), stride)) {
        error = "cannot write " + name;
        return false;
    }
    return true;
}

bool readPng(const fs::path &path, Picture &picture, std::string &error)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        error = "cannot read " + path.string();
        return false;
    }
    std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    int width = 0, height = 0, channels = 0;
    unsigned char *pixels = stbi_load_from_memory(bytes.data(), static_cast<int>(bytes.size()), &width, &height,
                                                  &channels, 3);
    if (!pixels) {
        error = "not a PNG: " + path.string();
        return false;
    }
    picture.width = static_cast<uint32_t>(width);
    picture.height = static_cast<uint32_t>(height);
    picture.rgb.assign(pixels, pixels + static_cast<size_t>(width) * height * 3);
    stbi_image_free(pixels);
    return true;
}

// ---- comparison ----

// The largest difference of any channel of one pixel
int channelDifference(const Picture &a, const Picture &b, size_t pixel)
{
    int worst = 0;
    for (size_t c = 0; c < 3; ++c) {
        const int d = std::abs(static_cast<int>(a.rgb[pixel * 3 + c]) - static_cast<int>(b.rgb[pixel * 3 + c]));
        worst = std::max(worst, d);
    }
    return worst;
}

struct Comparison {
    int maxDifference = 0;
    size_t beyond = 0; // pixels with a channel more than the tolerance off
    size_t pixels = 0;
    bool sameSize = false;
    bool matches() const { return sameSize && static_cast<double>(beyond) <= kMaxBeyondShare * static_cast<double>(pixels); }
};

Comparison compare(const Picture &actual, const Picture &golden)
{
    Comparison c;
    c.pixels = static_cast<size_t>(actual.width) * actual.height;
    c.sameSize = actual.width == golden.width && actual.height == golden.height;
    if (!c.sameSize)
        return c;
    for (size_t p = 0; p < c.pixels; ++p) {
        const int d = channelDifference(actual, golden, p);
        c.maxDifference = std::max(c.maxDifference, d);
        if (d > kTolerance)
            ++c.beyond;
    }
    return c;
}

// The actual picture faded to a quarter, with the pixels beyond the tolerance in bright red
Picture diffPicture(const Picture &actual, const Picture &golden)
{
    Picture diff;
    diff.width = actual.width;
    diff.height = actual.height;
    diff.rgb.resize(actual.rgb.size());
    const bool sameSize = actual.width == golden.width && actual.height == golden.height;
    const size_t pixels = static_cast<size_t>(actual.width) * actual.height;
    for (size_t p = 0; p < pixels; ++p) {
        const bool beyond = sameSize && channelDifference(actual, golden, p) > kTolerance;
        for (size_t c = 0; c < 3; ++c) {
            const size_t k = p * 3 + c;
            diff.rgb[k] = beyond ? static_cast<uint8_t>(c == 0 ? 255 : 0) : static_cast<uint8_t>(actual.rgb[k] / 4);
        }
    }
    return diff;
}

// ---- rendering one scene ----

// Turns the renderer's BGRA pixels into RGB
Picture fromBgra(uint32_t width, uint32_t height, const std::vector<uint8_t> &bgra)
{
    if (bgra.size() != static_cast<size_t>(width) * height * 4)
        throw std::runtime_error("the off-screen picture has the wrong size");
    Picture picture;
    picture.width = width;
    picture.height = height;
    picture.rgb.resize(static_cast<size_t>(width) * height * 3);
    for (size_t p = 0; p < static_cast<size_t>(width) * height; ++p) {
        picture.rgb[p * 3 + 0] = bgra[p * 4 + 2]; // red
        picture.rgb[p * 3 + 1] = bgra[p * 4 + 1]; // green
        picture.rgb[p * 3 + 2] = bgra[p * 4 + 0]; // blue
    }
    return picture;
}

// Builds a tree into a context on a hidden window of the given size, settles it, and draws it
// off screen. The context dies before the renderer and the window; whatever `build` captured
// (stores, views' models, images) must outlive this call.
Picture renderPicture(uint32_t width, uint32_t height, const ui::Theme &theme,
                      const std::function<void(ui::Context &)> &build)
{
    engine::Window window(width, height, "Vibin render test", false);
    window.setPosition(-32000, -32000); // far off the desktop, so the real mouse never hovers it
    engine::Renderer renderer(window, false, true);

    Picture picture;
    {
        ui::Context context(window, renderer, theme);
        build(context);

        // Frames until the picture stops changing: the same fingerprint three frames running
        uint64_t last = 0;
        int same = 0;
        for (int i = 0; i < kSettleFrames && same < kSteadyFrames; ++i) {
            context.update();
            const uint64_t print = context.buildFrame();
            same = (i > 0 && print == last) ? same + 1 : 1;
            last = print;
        }

        int fbWidth = 0, fbHeight = 0;
        window.framebufferSize(fbWidth, fbHeight);
        renderer.beginOffscreen(static_cast<uint32_t>(fbWidth), static_cast<uint32_t>(fbHeight));
        context.submit();
        const std::vector<uint8_t> bgra = renderer.endOffscreen();
        picture = fromBgra(static_cast<uint32_t>(fbWidth), static_cast<uint32_t>(fbHeight), bgra);
        picture.stats = context.stats();
    }
    return picture;
}

// Paints with the theme's helpers over the whole window (the raw drawing scenes)
using Painter = std::function<void(ui::DrawList &, const ui::Theme &, const ui::Rect &)>;

class Painted : public ui::Component {
public:
    explicit Painted(Painter paint) : paint_(std::move(paint)) {}

    ui::Vec2 measure(const ui::Theme &) const override { return {}; }
    void draw(ui::DrawList &list, const ui::Theme &theme) const override { paint_(list, theme, rect()); }

private:
    Painter paint_;
};

// Opens an overlay that draws `paint` over the window
void paintOverlay(ui::Context &context, Painter paint)
{
    context.openOverlay<Painted>(ui::OverlayOptions{false, false, {}}, std::move(paint));
}

// Stretches a component to fill its column or row of the root
void stretch(ui::Component &component)
{
    component.setGrow(1.0f);
    component.setAlignSelf(ui::Align::Stretch);
}

// A row container, for controls side by side
ui::Container &rowOf(ui::Container &parent)
{
    return parent.add<ui::Container>(ui::Direction::Row);
}

// The DAW's hooks, each a harmless stand-in (nothing plays, nothing opens)
daw::TransportHooks quietTransport()
{
    daw::TransportHooks hooks;
    hooks.position = [] { return 0.0; };
    hooks.playing = [] { return false; };
    hooks.seek = [](double) {};
    hooks.openClip = [] {};
    hooks.audioFor = [](int, int) -> const audio::AudioData * { return nullptr; };
    hooks.isRecording = [](int, int) { return false; };
    hooks.trackInput = [](int) { return std::string(); };
    hooks.trackListening = [](int) { return false; };
    hooks.clipMenu = [](ui::Vec2, double) {};
    hooks.trackMenu = [](int, ui::Vec2) {};
    hooks.chooseInstrument = [](int, ui::Rect) {};
    return hooks;
}

daw::PianoRollHooks quietPianoRoll()
{
    daw::PianoRollHooks hooks;
    hooks.auditionOn = [](int, float) {};
    hooks.auditionOff = [](int) {};
    hooks.position = [] { return 0.0; };
    hooks.seek = [](double) {};
    hooks.playing = [] { return false; };
    hooks.contextMenu = [](ui::Vec2) {};
    hooks.laneMenu = [](ui::Vec2) {};
    return hooks;
}

daw::StepSequencerHooks quietSteps()
{
    daw::StepSequencerHooks hooks;
    hooks.auditionOn = [](int, float) {};
    hooks.auditionOff = [](int) {};
    hooks.position = [] { return 0.0; };
    hooks.playing = [] { return false; };
    hooks.rowMenu = [](int, ui::Vec2) {};
    hooks.addRowMenu = [](ui::Vec2) {};
    return hooks;
}

// The demo's first instrument track's first clip, selected
void selectFirstInstrumentClip(daw::ProjectStore &store)
{
    const daw::Project &project = store.project();
    for (size_t t = 0; t < project.tracks.size(); ++t) {
        if (project.tracks[t].kind == daw::TrackKind::Instrument && !project.tracks[t].clips.empty()) {
            store.select(static_cast<int>(t), 0);
            return;
        }
    }
}

// ---- scenes: the raw drawing ones ----

void paintShapes(ui::DrawList &list, const ui::Theme &theme, const ui::Rect &)
{
    const ui::Shape shapes[] = {ui::Shape::Rounded,       ui::Shape::Soft,         ui::Shape::Pill,
                                ui::Shape::Circle,        ui::Shape::TriangleRight, ui::Shape::TriangleLeft,
                                ui::Shape::TriangleDown,  ui::Shape::Cross,        ui::Shape::TriangleUp};
    const ui::Elevation elevations[] = {ui::Elevation::Raised, ui::Elevation::Flat, ui::Elevation::Inset};
    const ui::Finish finishes[] = {ui::Finish::Flat, ui::Finish::Convex, ui::Finish::Concave};
    const int shapeCount = 9;
    const float left = 96.0f, top = 40.0f, cellW = 56.0f, cellH = 40.0f;

    // Each column is one elevation and finish; each row one shape, at depth scales 0.5 and 1
    int column = 0;
    for (ui::Elevation elevation : elevations) {
        for (ui::Finish finish : finishes) {
            for (int row = 0; row < shapeCount; ++row) {
                const float x = left + column * cellW;
                const float y = top + row * cellH;
                list.add(theme.surface(box(x + 4, y + 8, 22, 22), shapes[row], elevation, 0.5f, finish));
                list.add(theme.surface(box(x + 28, y + 8, 22, 22), shapes[row], elevation, 1.0f, finish));
            }
            ++column;
        }
    }

    // Each row's accent and ink, on the left
    for (int row = 0; row < shapeCount; ++row) {
        const float y = top + row * cellH;
        list.add(theme.accent(box(16, y + 10, 20, 20), shapes[row]));
        list.add(theme.ink(box(48, y + 10, 20, 20), shapes[row]));
    }

    // Along the bottom: raised surfaces, an accent pill, and scrims over them
    for (int i = 0; i < 4; ++i)
        list.add(theme.surface(box(96 + i * 120.0f, 424, 100, 36), ui::Shape::Rounded, ui::Elevation::Raised, 1.0f,
                               ui::Finish::Convex));
    list.add(theme.accent(box(16, 428, 60, 28), ui::Shape::Pill));
    list.add(theme.scrim(box(0, 412, 320, 68), 0.5f));
    list.add(theme.scrim(box(320, 412, 320, 68), 0.25f));
}

void paintText(ui::DrawList &list, const ui::Theme &theme, const ui::Rect &)
{
    const ui::TextStyle styles[] = {ui::TextStyle::Caption, ui::TextStyle::Body,     ui::TextStyle::Subtitle,
                                    ui::TextStyle::Title,   ui::TextStyle::Headline, ui::TextStyle::Display};
    const ui::TextTone tones[] = {ui::TextTone::Primary, ui::TextTone::Secondary, ui::TextTone::Muted,
                                  ui::TextTone::Accent,  ui::TextTone::OnAccent};

    // Every style in a row, every tone across it (OnAccent on an accent fill, as it is used)
    for (int s = 0; s < 6; ++s) {
        const float y = 8.0f + s * 52.0f;
        for (int t = 0; t < 5; ++t) {
            const ui::Rect cell = box(20.0f + t * 124.0f, y, 116, 48);
            if (tones[t] == ui::TextTone::OnAccent)
                list.add(theme.accent(cell, ui::Shape::Rounded));
            list.add(theme.text("Abc", cell, styles[s], tones[t], ui::TextAlign::Center));
        }
    }

    // The three alignments, in a box as wide as the picture
    list.add(theme.text("Aligned to the start", box(20, 336, 600, 22), ui::TextStyle::Body, ui::TextTone::Primary,
                        ui::TextAlign::Start));
    list.add(theme.text("Aligned to the centre", box(20, 360, 600, 22), ui::TextStyle::Body, ui::TextTone::Primary,
                        ui::TextAlign::Center));
    list.add(theme.text("Aligned to the end", box(20, 384, 600, 22), ui::TextStyle::Body, ui::TextTone::Primary,
                        ui::TextAlign::End));

    // A long line, cut short by its clip
    list.pushClip(box(20, 420, 300, 24));
    list.add(theme.text("A long line of text that the clip cuts short at its right edge, not wrapped",
                        box(20, 420, 600, 24), ui::TextStyle::Body, ui::TextTone::Secondary, ui::TextAlign::Start));
    list.popClip();
}

void paintClips(ui::DrawList &list, const ui::Theme &theme, const ui::Rect &)
{
    list.add(theme.surface(box(30, 30, 420, 300), ui::Shape::Rounded, ui::Elevation::Raised));

    // The first clip cuts the raised surfaces' shadows at its edges
    list.pushClip(box(50, 50, 380, 260));
    list.add(theme.surface(box(20, 40, 180, 120), ui::Shape::Rounded, ui::Elevation::Raised));
    list.add(theme.text("Clipped text", box(60, 70, 160, 30), ui::TextStyle::Body, ui::TextTone::Primary,
                        ui::TextAlign::Start));

    // A nested clip cuts another raised surface and an inset well
    list.pushClip(box(120, 120, 300, 200));
    list.add(theme.surface(box(260, 150, 200, 120), ui::Shape::Rounded, ui::Elevation::Raised, 1.0f, ui::Finish::Convex));
    list.add(theme.surface(box(140, 190, 120, 80), ui::Shape::Rounded, ui::Elevation::Inset));
    list.add(theme.text("Nested", box(140, 190, 120, 80), ui::TextStyle::Subtitle, ui::TextTone::Accent,
                        ui::TextAlign::Center));
    list.popClip();
    list.popClip();

    list.add(theme.text("Outside the clips", box(40, 330, 300, 20), ui::TextStyle::Caption, ui::TextTone::Muted,
                        ui::TextAlign::Start));
}

// A generated picture: gradients in red and green, a checker in blue
void makeImage(ui::ImageData &image)
{
    const uint32_t width = 96, height = 64;
    image.width = width;
    image.height = height;
    image.pixels.assign(static_cast<size_t>(width) * height * 4, 0);
    for (uint32_t y = 0; y < height; ++y) {
        for (uint32_t x = 0; x < width; ++x) {
            const size_t i = (static_cast<size_t>(y) * width + x) * 4;
            const bool checker = ((x / 8) + (y / 8)) % 2 == 1;
            image.pixels[i + 0] = checker ? 235 : 30;                                  // blue
            image.pixels[i + 1] = static_cast<uint8_t>(y * 255 / (height - 1));        // green
            image.pixels[i + 2] = static_cast<uint8_t>(x * 255 / (width - 1));         // red
            image.pixels[i + 3] = 255;                                                 // unused
        }
    }
    image.version = 1;
}

// ---- scenes ----

Picture sceneShapes()
{
    return renderPicture(640, 480, ui::Theme::light(), [](ui::Context &context) { paintOverlay(context, paintShapes); });
}

Picture sceneShapesDark()
{
    return renderPicture(640, 480, daw::studioTheme(true),
                         [](ui::Context &context) { paintOverlay(context, paintShapes); });
}

Picture sceneText()
{
    return renderPicture(640, 480, daw::studioTheme(true), [](ui::Context &context) { paintOverlay(context, paintText); });
}

Picture sceneClips()
{
    return renderPicture(480, 360, daw::studioTheme(true), [](ui::Context &context) { paintOverlay(context, paintClips); });
}

Picture sceneImage()
{
    ui::ImageData image; // kept alive while the frame is drawn
    makeImage(image);
    return renderPicture(480, 360, daw::studioTheme(true), [&image](ui::Context &context) {
        paintOverlay(context, [&image](ui::DrawList &list, const ui::Theme &theme, const ui::Rect &) {
            // At its own size
            list.add(ui::ImageDraw{box(20, 20, 96, 64), &image, 1.0f});
            // Stretched
            list.add(ui::ImageDraw{box(140, 20, 320, 180), &image, 1.0f});
            // At half opacity over a raised surface, clipped to its left part
            list.add(theme.surface(box(20, 200, 240, 140), ui::Shape::Rounded, ui::Elevation::Raised));
            list.pushClip(box(20, 200, 160, 140));
            list.add(ui::ImageDraw{box(20, 200, 240, 140), &image, 0.5f});
            list.popClip();
            list.add(theme.text("Half opacity, clipped", box(270, 200, 190, 140), ui::TextStyle::Body,
                                ui::TextTone::Secondary, ui::TextAlign::Start));
        });
    });
}

// ---- scenes: component trees ----

void buildControls(ui::Context &context)
{
    ui::Container &root = context.root();
    root.setDirection(ui::Direction::Column);
    ui::Grid &grid = root.add<ui::Grid>(3);
    stretch(grid);

    // Column one: buttons, toggles and the value controls
    ui::Panel &left = grid.add<ui::Panel>(ui::Direction::Column);
    left.add<ui::Label>("Buttons", ui::TextStyle::Subtitle);
    const std::pair<ui::ButtonVariant, const char *> variants[] = {{ui::ButtonVariant::Raised, "Raised"},
                                                                   {ui::ButtonVariant::Flat, "Flat"},
                                                                   {ui::ButtonVariant::Accent, "Accent"},
                                                                   {ui::ButtonVariant::Inset, "Inset"}};
    const std::pair<ui::Size, const char *> sizes[] = {
        {ui::Size::Small, "Small"}, {ui::Size::Medium, "Medium"}, {ui::Size::Large, "Large"}};
    for (const auto &variant : variants) {
        ui::Container &row = rowOf(left);
        for (const auto &size : sizes)
            row.add<ui::Button>().setLabel(size.second).setVariant(variant.first).setSize(size.first);
    }
    ui::Container &icons = rowOf(left);
    icons.add<ui::Button>().setIcon(ui::Icon::Play).setSelected(true);
    icons.add<ui::Button>().setIcon(ui::Icon::Chevron);
    icons.add<ui::Button>().setIcon(ui::Icon::Gear).setVariant(ui::ButtonVariant::Accent);

    ui::Container &toggles = rowOf(left);
    toggles.add<ui::Toggle>(true);
    toggles.add<ui::Toggle>(false);
    left.add<ui::Slider>(0.35f);
    left.add<ui::Checkbox>("Checked", true);
    left.add<ui::Checkbox>("Unchecked", false);
    left.add<ui::RadioGroup>(std::vector<std::string>{"Sine", "Saw", "Square"}, 1);
    left.add<ui::SegmentedControl>(std::vector<std::string>{"Off", "Low", "High"}, 1);
    left.add<ui::Stepper>(4.0f, 0.0f, 10.0f, 1.0f).setSuffix(" bars");

    // Column two: knobs, bars, text and tags
    ui::Panel &middle = grid.add<ui::Panel>(ui::Direction::Column);
    middle.add<ui::Label>("Values", ui::TextStyle::Subtitle);
    ui::Container &knobs = rowOf(middle);
    const auto percent = [](float v) { return std::to_string(static_cast<int>(v * 100.0f + 0.5f)) + "%"; };
    for (float v : {0.2f, 0.6f, 0.9f})
        knobs.add<ui::Knob>(v).setDiameter(56.0f).setFormatter(percent);
    middle.add<ui::ProgressBar>(0.7f);
    ui::TextField &field = middle.add<ui::TextField>("Search");
    field.setText("kick drum");
    ui::Container &chips = rowOf(middle);
    chips.add<ui::Chip>("Drums", true);
    chips.add<ui::Chip>("Bass", false);
    chips.add<ui::Chip>("Lead", false);
    chips.add<ui::Badge>("12");
    ui::LevelMeter &meter = middle.add<ui::LevelMeter>(ui::Direction::Row);
    meter.setLevel(0.7f);

    // Column three: list rows and panels
    ui::Panel &right = grid.add<ui::Panel>(ui::Direction::Column);
    right.add<ui::Label>("Lists", ui::TextStyle::Subtitle);
    right.add<ui::ListItem>("Kick", [] {}).setSubtitle("Drum kit").setTrailing("36").setSelected(true);
    right.add<ui::ListItem>("Snare", [] {}).setSubtitle("Drum kit").setTrailing("38");
    right.add<ui::Divider>();
    ui::Panel &card = right.add<ui::Panel>(ui::Direction::Column);
    card.add<ui::Label>("A raised panel", ui::TextStyle::Body);
    card.add<ui::Label>("with a label inside", ui::Size::Small);
    ui::Panel &well = right.add<ui::Panel>(ui::Direction::Column, ui::Elevation::Inset);
    well.add<ui::Label>("An inset well", ui::Size::Medium);
}

// The Context over an arrangement, a piano roll, a step sequencer or a synth, each filling the window

Picture sceneControls()
{
    return renderPicture(900, 600, daw::studioTheme(true), buildControls);
}

Picture sceneArrangement()
{
    daw::ProjectStore store(daw::demoProject()); // outlives the context
    return renderPicture(1200, 520, daw::studioTheme(true), [&store](ui::Context &context) {
        stretch(context.root().add<daw::ArrangementView>(store, quietTransport()));
    });
}

Picture scenePianoRoll()
{
    daw::ProjectStore store(daw::demoProject());
    selectFirstInstrumentClip(store);
    return renderPicture(1200, 520, daw::studioTheme(true), [&store](ui::Context &context) {
        stretch(context.root().add<daw::PianoRollView>(store, quietPianoRoll()));
    });
}

Picture sceneSteps()
{
    // A drum clip: a kick on the beats 0, 2, 4 and 6, a snare on 1, 3, 5 and 7, hats every half beat
    daw::Project project;
    project.name = "Steps";
    std::vector<daw::Note> notes;
    auto hit = [&notes](int key, double start) {
        daw::Note n;
        n.start = start;
        n.length = 0.25;
        n.key = key;
        n.velocity = 0.8f;
        notes.push_back(n);
    };
    for (int beat = 0; beat < 8; beat += 2)
        hit(36, beat);
    for (int beat = 1; beat < 8; beat += 2)
        hit(38, beat);
    for (int step = 0; step < 16; ++step)
        hit(42, step * 0.5);
    std::stable_sort(notes.begin(), notes.end(), [](const daw::Note &a, const daw::Note &b) { return a.start < b.start; });

    daw::Track drums;
    drums.name = "Drums";
    drums.kind = daw::TrackKind::Instrument;
    drums.clips.push_back(daw::Clip("Beat", 0.0, 8.0, notes));
    project.tracks.push_back(drums);

    daw::ProjectStore store(std::move(project));
    store.select(0, 0);
    return renderPicture(1000, 420, daw::studioTheme(true), [&store](ui::Context &context) {
        daw::StepSequencerView &view = context.root().add<daw::StepSequencerView>(store, quietSteps());
        stretch(view);
        view.selectRow(36);
    });
}

Picture sceneSynth()
{
    audio::DeviceSetting setting = audio::SynthDevice::setting("Init"); // outlives the context
    daw::DeviceAccess access;
    access.setting = [&setting] { return &setting; };
    access.edit = [&setting] { return &setting; };
    access.device = [] { return static_cast<audio::Device *>(nullptr); };
    return renderPicture(1180, 860, daw::studioTheme(true), [&access](ui::Context &context) {
        stretch(context.root().add<daw::SynthView>(context, access));
    });
}

// ---- primitives (no window) ----

struct Checker {
    int failures = 0;

    void check(bool ok, const std::string &what, const std::string &detail = {})
    {
        std::printf("%s  %-44s %s\n", ok ? "PASS" : "FAIL", what.c_str(), detail.c_str());
        if (!ok)
            ++failures;
    }
};

// Numbers as text, for a check's detail
std::string describe(const float *values, int count)
{
    std::string s;
    for (int i = 0; i < count; ++i) {
        char buf[32];
        std::snprintf(buf, sizeof buf, "%s%g", i ? " " : "", static_cast<double>(values[i]));
        s += buf;
    }
    return s;
}

bool near(float a, float b)
{
    return std::fabs(a - b) < 1e-4f;
}

bool isClip(const float *clip, float x0, float y0, float x1, float y1)
{
    return near(clip[0], x0) && near(clip[1], y0) && near(clip[2], x1) && near(clip[3], y1);
}

bool unclipped(const float *clip)
{
    return isClip(clip, -ui::kNoClip, -ui::kNoClip, ui::kNoClip, ui::kNoClip);
}

// One letter a prim: S surface, G glyph, I image, in drawing order
std::string kindsOf(const ui::DrawList &list)
{
    std::string s;
    for (const ui::Prim &p : list.prims()) {
        switch (static_cast<ui::PrimKind>(p.kind)) {
        case ui::PrimKind::Surface: s += 'S'; break;
        case ui::PrimKind::Glyph: s += 'G'; break;
        case ui::PrimKind::Image: s += 'I'; break;
        default: s += '?'; break;
        }
    }
    return s;
}

void primChecks(Checker &c)
{
    std::printf("Primitives: clips, surfaces, text, images and fingerprints, no window\n\n");
    const ui::Theme theme = ui::Theme::light(); // outlives the lists, which lay their text out with its font

    // Clips round out to whole pixels
    float clip[4];
    ui::clipBounds(box(10.3f, 20.7f, 5.2f, 5.2f), true, clip);
    c.check(isClip(clip, 10, 20, 16, 26), "A clip rounds out to whole pixels", describe(clip, 4));
    ui::clipBounds(box(10.3f, 20.7f, 5.2f, 5.2f), false, clip);
    c.check(unclipped(clip), "An unclipped rect gives the whole plane", describe(clip, 4));

    // Nested clips carry the rounded intersection; after both pops a surface is unclipped
    {
        ui::DrawList list;
        list.setFont(&theme.font());
        ui::Surface s;
        s.rect = box(0, 0, 40, 40);
        list.pushClip(box(0, 0, 100, 100));
        list.pushClip(box(50.4f, 20.2f, 100, 30.6f));
        list.add(s);
        list.popClip();
        list.popClip();
        list.add(s);
        c.check(list.prims().size() == 2 && isClip(list.prims()[0].clip, 50, 20, 100, 51),
                "Nested clips carry the rounded intersection", describe(list.prims()[0].clip, 4));
        c.check(list.prims().size() == 2 && unclipped(list.prims()[1].clip),
                "After both pops the next surface is unclipped");
    }

    // A surface packs as the shader reads it
    {
        ui::Surface s;
        s.rect = box(12, 14, 60, 30);
        s.radius = 6.0f;
        s.distance = 3.0f;
        s.blur = 4.0f;
        s.elevation = ui::Elevation::Inset;
        s.rim = 0.5f;
        s.base = ui::Color{0.8f, 0.7f, 0.6f, 1.0f};
        s.opacity = 0.9f;
        s.light = ui::Color{1.0f, 1.0f, 1.0f, 1.0f};
        s.dark = ui::Color{0.2f, 0.2f, 0.2f, 1.0f};
        s.intensity = 0.4f;
        s.curvature = 0.16f;
        s.form = 2.0f;
        const float clipIn[4] = {5, 5, 90, 90};
        const ui::Prim p = ui::surfacePrim(s, clipIn);

        c.check(p.kind == static_cast<uint32_t>(ui::PrimKind::Surface) && near(p.rect[0], 12) && near(p.rect[1], 14) &&
                    near(p.rect[2], 60) && near(p.rect[3], 30),
                "A surface prim has its kind and rect", describe(p.rect, 4));
        c.check(isClip(p.clip, 5, 5, 90, 90), "A surface prim carries the clip it was given", describe(p.clip, 4));
        c.check(near(p.shape[3], 2.225f), "The rim adds 0.45 of it to the elevation", describe(p.shape, 4));
        c.check(near(p.style[3], 2.0f) && near(p.base[3], 0.9f),
                "The form is in style and the opacity in base", describe(p.style, 4) + " / " + describe(p.base, 4));
        c.check(near(p.base[0], 0.8f) && near(p.base[1], 0.7f) && near(p.base[2], 0.6f),
                "A surface's colour is in base", describe(p.base, 3));
        c.check(p.pad[0] == 0 && p.pad[1] == 0 && p.pad[2] == 0, "The pad is zero");
    }

    // Text: one glyph prim per visible glyph, the colour in base
    ui::Text hello;
    hello.text = "Hello";
    hello.baseline = {20.0f, 30.0f};
    hello.size = 13.0f;
    hello.color = ui::Color{0.1f, 0.2f, 0.3f, 1.0f};
    {
        ui::DrawList list;
        list.setFont(&theme.font());
        list.add(hello);
        c.check(kindsOf(list) == "GGGGG", "Text adds one glyph prim per visible glyph", kindsOf(list));
        c.check(list.prims().size() == 5 && near(list.prims()[0].base[0], 0.1f) &&
                    near(list.prims()[0].base[1], 0.2f) && near(list.prims()[0].base[2], 0.3f),
                "Glyphs carry the text's colour in base");
    }
    {
        ui::DrawList list;
        list.setFont(&theme.font());
        ui::Text spaced = hello;
        spaced.text = "a b";
        list.add(spaced);
        // One prim per quad the font lays out (stb gives the space a blank quad of its own)
        std::vector<ui::GlyphQuad> quads;
        theme.font().layout(spaced.text, spaced.baseline, spaced.size, quads);
        c.check(list.prims().size() == quads.size() && !quads.empty(), "A text adds one glyph per laid-out quad",
                kindsOf(list));
    }
    {
        ui::DrawList list;
        list.setFont(&theme.font());
        ui::Text empty = hello;
        empty.text = "";
        list.add(empty);
        c.check(list.empty(), "An empty text adds nothing");
    }
    {
        ui::DrawList list; // no font set
        list.add(hello);
        c.check(list.empty(), "Text with no font adds nothing");
    }
    {
        ui::DrawList list;
        list.setFont(&theme.font());
        ui::Surface s;
        s.rect = box(0, 0, 40, 40);
        list.add(s);
        list.add(hello);
        list.add(s);
        c.check(kindsOf(list) == "SGGGGGS", "Prims stay in drawing order across kinds", kindsOf(list));
        c.check(list.counts().surfaces == 2 && list.counts().texts == 1 && list.counts().glyphs == 5 &&
                    list.counts().images == 0,
                "Counts give surfaces, texts and glyphs");
    }

    // Images: nothing without pixels or size; a real one is one prim, referenced by its index
    {
        ui::DrawList list;
        list.setFont(&theme.font());
        list.add(ui::ImageDraw{box(0, 0, 8, 8), nullptr, 1.0f});
        ui::ImageData zero; // no size
        list.add(ui::ImageDraw{box(0, 0, 8, 8), &zero, 1.0f});
        c.check(list.empty() && list.images().empty(), "An image with no pixels or size adds nothing");
    }
    ui::ImageData picture;
    picture.width = 8;
    picture.height = 8;
    picture.pixels.assign(8 * 8 * 4, 128);
    picture.version = 1;
    {
        ui::DrawList list;
        list.setFont(&theme.font());
        ui::Surface s;
        s.rect = box(0, 0, 40, 40);
        list.add(s);
        list.add(ui::ImageDraw{box(0, 0, 8, 8), &picture, 1.0f});
        c.check(kindsOf(list) == "SI" && list.images().size() == 1 && list.images()[0].prim == 1 &&
                    list.images()[0].image == &picture && list.counts().images == 1,
                "An image adds one prim and a reference to its index", kindsOf(list));
    }

    // Fingerprints: the same lists agree; an image's version and a clear show through
    {
        ui::Surface s;
        s.rect = box(10, 10, 100, 40);
        auto make = [&] {
            ui::DrawList list;
            list.setFont(&theme.font());
            list.add(s);
            list.add(hello);
            list.add(ui::ImageDraw{box(0, 0, 8, 8), &picture, 1.0f});
            return ui::fingerprint(list);
        };
        const uint64_t first = make();
        c.check(make() == first, "Two lists built the same have equal fingerprints");
        picture.version = 2;
        c.check(make() != first, "Bumping an image's version changes the fingerprint");
        picture.version = 1;

        ui::DrawList list;
        list.setFont(&theme.font());
        list.add(s);
        list.add(hello);
        list.clear();
        c.check(list.empty() && list.counts().glyphs == 0, "clear empties the list and its counts");
        list.add(hello);
        c.check(kindsOf(list) == "GGGGG", "clear keeps the font", kindsOf(list));
    }
    std::printf("\n");
}

// ---- checking ----

struct SceneEntry {
    const char *name;
    Picture (*render)();
};

const SceneEntry kScenes[] = {
    {"shapes", sceneShapes},
    {"shapes-dark", sceneShapesDark},
    {"text", sceneText},
    {"clips", sceneClips},
    {"image", sceneImage},
    {"controls", sceneControls},
    {"arrangement", sceneArrangement},
    {"pianoroll", scenePianoRoll},
    {"steps", sceneSteps},
    {"synth", sceneSynth},
};

// Renders one scene and checks it; returns its failures and keeps its frame stats
int checkScene(const std::string &name, Picture (*render)(), bool update, std::vector<std::pair<std::string, ui::FrameStats>> &rendered)
{
    try {
        const Picture actual = render();
        rendered.emplace_back(name, actual.stats);
        const fs::path golden = goldenPath(name);
        std::string error;

        if (update) {
            if (!writePng(golden, actual, error)) {
                std::printf("FAIL  %-12s %s\n", name.c_str(), error.c_str());
                return 1;
            }
            std::printf("PASS  %-12s updated %s\n", name.c_str(), golden.string().c_str());
            return 0;
        }

        if (!fs::exists(golden)) {
            std::printf("FAIL  %-12s no golden at %s; run with --update-goldens\n", name.c_str(), golden.string().c_str());
            return 1;
        }
        Picture expected;
        if (!readPng(golden, expected, error)) {
            std::printf("FAIL  %-12s %s\n", name.c_str(), error.c_str());
            return 1;
        }

        const Comparison c = compare(actual, expected);
        if (c.matches()) {
            std::printf("PASS  %-12s matches its golden (max channel difference %d, %zu pixels beyond %d)\n",
                        name.c_str(), c.maxDifference, c.beyond, kTolerance);
            return 0;
        }

        // Mismatch: write the picture and the diff for looking at
        const fs::path folder = outputFolder();
        const fs::path actualPath = folder / (name + ".actual.png");
        const fs::path diffPath = folder / (name + ".diff.png");
        std::string writeError;
        writePng(actualPath, actual, writeError);
        writePng(diffPath, diffPicture(actual, expected), writeError);

        const std::string reason = c.sameSize ? "" : "size " + std::to_string(actual.width) + "x" +
                                                       std::to_string(actual.height) + ", golden is " +
                                                       std::to_string(expected.width) + "x" +
                                                       std::to_string(expected.height);
        std::printf("FAIL  %-12s max channel difference %d, %zu pixels beyond %d (of %zu) %s; actual %s, diff %s%s%s\n",
                    name.c_str(), c.maxDifference, c.beyond, kTolerance, c.pixels, reason.c_str(),
                    actualPath.string().c_str(), diffPath.string().c_str(), writeError.empty() ? "" : "; ",
                    writeError.c_str());
        return 1;
    } catch (const std::exception &e) {
        std::printf("FAIL  %-12s render failed: %s\n", name.c_str(), e.what());
        return 1;
    } catch (...) {
        std::printf("FAIL  %-12s render failed\n", name.c_str());
        return 1;
    }
}

void printInfo(const std::string &name, const ui::FrameStats &s)
{
    std::printf("INFO render %s: build %.2f ms, %u surfaces, %u texts, %u glyphs, %u prims, %u draws, %zu KB, submit %.2f ms\n",
                name.c_str(), s.buildMs, s.surfaces, s.texts, s.glyphs, s.surfaces + s.glyphs + s.images, s.draws,
                s.uploadBytes / 1024, s.submitMs);
}

} // namespace

int runRenderSelfTest(int argc, char **argv)
{
    bool update = false;
    std::string only;
    int failures = 0;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--update-goldens")
            update = true;
        else if (arg == "--scene" && i + 1 < argc)
            only = argv[++i];
        else if (arg == "--scene") {
            std::printf("FAIL  --scene needs a name\n");
            ++failures;
        }
    }

    // The primitive checks need no window and run whatever scene is asked for
    Checker prims;
    primChecks(prims);
    failures += prims.failures;

    bool found = only.empty();
    std::vector<std::pair<std::string, ui::FrameStats>> rendered;
    for (const SceneEntry &scene : kScenes) {
        if (!only.empty() && only != scene.name)
            continue;
        found = true;
        failures += checkScene(scene.name, scene.render, update, rendered);
    }
    if (!found) {
        std::printf("FAIL  unknown scene \"%s\"\n", only.c_str());
        ++failures;
    }

    for (const auto &entry : rendered)
        printInfo(entry.first, entry.second);
    return failures;
}

}
