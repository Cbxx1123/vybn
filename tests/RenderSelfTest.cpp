// Scenes, each described in RenderSelfTest.h: shapes, shapes-dark, text, clips, image, lines, plots,
// controls, arrangement, waveforms, pianoroll, steps, scroll (a 2000-row ScrollView, to show culling at work)
// and synth. The synth and waveforms scenes are also timed over 50 settled frames (INFO lines after the scenes).
#include "RenderSelfTest.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "audio/AudioFile.h"
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
#include "ui/DrawCache.h"
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
const int kTimedFrames = 50;         // the synth scene's timed frames (build and submit, averaged)

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

// What the timed frames of a settled scene cost, on average (the readback isn't timed)
struct Timing {
    int frames = 0;
    double buildMs = 0.0;
    double submitMs = 0.0;
    uint32_t prims = 0;
    size_t uploadBytes = 0;
};

// Builds a tree into a context on a hidden window of the given size, settles it, and draws it
// off screen. With `timedFrames` and `timing`, the settled scene first runs that many frames of
// update, buildFrame and submit, and the averages go to `timing`. The context dies before the
// renderer and the window; whatever `build` captured (stores, views' models, images) must outlive
// this call.
Picture renderPicture(uint32_t width, uint32_t height, const ui::Theme &theme,
                      const std::function<void(ui::Context &)> &build, int timedFrames = 0,
                      Timing *timing = nullptr, ui::FrameStats *afterFrame = nullptr)
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
        const uint32_t fbW = static_cast<uint32_t>(fbWidth), fbH = static_cast<uint32_t>(fbHeight);

        if (timedFrames > 0 && timing) {
            double buildSum = 0.0, submitSum = 0.0;
            size_t bytesSum = 0;
            for (int i = 0; i < timedFrames; ++i) {
                context.update();
                context.buildFrame();
                renderer.beginOffscreen(fbW, fbH);
                context.submit();
                renderer.endOffscreen(); // the readback is not timed
                const ui::FrameStats &s = context.stats();
                buildSum += s.buildMs;
                submitSum += s.submitMs;
                bytesSum += s.uploadBytes;
                timing->prims = s.surfaces + s.glyphs + s.images;
            }
            timing->frames = timedFrames;
            timing->buildMs = buildSum / timedFrames;
            timing->submitMs = submitSum / timedFrames;
            timing->uploadBytes = bytesSum / static_cast<size_t>(timedFrames);
        }

        renderer.beginOffscreen(fbW, fbH);
        context.submit();
        const std::vector<uint8_t> bgra = renderer.endOffscreen();
        picture = fromBgra(static_cast<uint32_t>(fbWidth), static_cast<uint32_t>(fbHeight), bgra);
        picture.stats = context.stats();

        // With afterFrame, one more update and buildFrame on the settled scene; their stats go there
        if (afterFrame) {
            context.update();
            context.buildFrame();
            *afterFrame = context.stats();
        }
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

// Lines: rows at five widths and four angles (a row at opacity 0.3), a sine and a zigzag polyline,
// a line cut by a clip and a dot
void paintLines(ui::DrawList &list, const ui::Theme &theme, const ui::Rect &)
{
    const float widths[] = {1.0f, 1.5f, 2.0f, 3.0f, 6.0f};
    const char *captions[] = {"1 px", "1.5 px", "2 px", "3 px", "6 px"};
    const float angles[] = {0.0f, 15.0f, 45.0f, 90.0f};
    const float columns[] = {60.0f, 150.0f, 240.0f, 330.0f};
    const float half = 18.0f; // half a line's length
    const float pi = 3.14159265f;

    // A row's four lines, each centred on its column at its angle
    auto row = [&](float y, float width, float opacity) {
        for (int j = 0; j < 4; ++j) {
            const float radians = angles[j] * pi / 180.0f;
            const ui::Vec2 centre{columns[j], y};
            const ui::Vec2 a{centre.x - half * std::cos(radians), centre.y - half * std::sin(radians)};
            const ui::Vec2 b{centre.x + half * std::cos(radians), centre.y + half * std::sin(radians)};
            list.add(theme.inkLine(a, b, width, ui::TextTone::Primary, opacity));
        }
    };

    for (int i = 0; i < 5; ++i) {
        const float y = 50.0f + i * 36.0f;
        row(y, widths[i], 1.0f);
        list.add(theme.text(captions[i], box(420, y - 10, 200, 20), ui::TextStyle::Caption, ui::TextTone::Secondary,
                            ui::TextAlign::Start));
    }
    row(240.0f, 3.0f, 0.3f);
    list.add(theme.text("3 px at 0.3 opacity", box(420, 230, 200, 20), ui::TextStyle::Caption,
                        ui::TextTone::Secondary, ui::TextAlign::Start));

    // A sine of two cycles over 500 px, 60 points, in the accent
    std::vector<ui::Vec2> wave;
    const int samples = 60;
    for (int i = 0; i < samples; ++i) {
        const float t = static_cast<float>(i) / (samples - 1);
        wave.push_back({70.0f + t * 500.0f, 300.0f - 28.0f * std::sin(t * 2.0f * 2.0f * pi)});
    }
    list.addPolyline(wave.data(), wave.size(), theme.accentLine({0.0f, 0.0f}, {0.0f, 0.0f}, 2.0f));

    // A zigzag in ink
    std::vector<ui::Vec2> zigzag;
    for (int i = 0; i < 25; ++i)
        zigzag.push_back({70.0f + i * 20.0f, i % 2 == 0 ? 360.0f : 404.0f});
    list.addPolyline(zigzag.data(), zigzag.size(), theme.inkLine({0.0f, 0.0f}, {0.0f, 0.0f}, 1.5f));

    // A line cut short by its clip, and a dot of width 8
    list.pushClip(box(200, 420, 200, 40));
    list.add(theme.inkLine({40.0f, 440.0f}, {560.0f, 440.0f}, 6.0f));
    list.popClip();
    list.add(theme.accentLine({600.0f, 450.0f}, {600.0f, 450.0f}, 8.0f));
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

Picture sceneLines()
{
    return renderPicture(640, 480, daw::studioTheme(true), [](ui::Context &context) { paintOverlay(context, paintLines); });
}

// Seeded noise in 0 to 1, the same every run (a linear congruential generator)
float seededNoise(uint32_t &state)
{
    state = state * 1664525u + 1013904223u;
    return static_cast<float>(state >> 8) / 16777216.0f;
}

Picture scenePlots()
{
    const float pi = 3.14159265f;

    // A sine of two cycles, 128 values
    std::vector<float> sine(128);
    for (size_t i = 0; i < sine.size(); ++i)
        sine[i] = std::sin(static_cast<float>(i) / 127.0f * 2.0f * 2.0f * pi);

    // A decaying envelope, 64 values from 1 down to near 0
    std::vector<float> envelope(64);
    for (size_t i = 0; i < envelope.size(); ++i)
        envelope[i] = std::exp(-4.0f * static_cast<float>(i) / 63.0f);

    // A waveform: 300 columns of seeded noise shaped by a swelling envelope, as low and high pairs
    std::vector<float> waveform(600);
    uint32_t seed = 2026;
    for (size_t column = 0; column < 300; ++column) {
        const float shape = std::sin(pi * static_cast<float>(column) / 299.0f);
        const float amplitude = shape * (0.15f + 0.85f * seededNoise(seed));
        waveform[2 * column] = -amplitude * (0.6f + 0.4f * seededNoise(seed));     // low
        waveform[2 * column + 1] = amplitude * (0.6f + 0.4f * seededNoise(seed)); // high
    }

    // A fast sine plus a slow one, 4096 values
    std::vector<float> dense(4096);
    for (size_t i = 0; i < dense.size(); ++i) {
        const float t = static_cast<float>(i) / 4095.0f;
        dense[i] = 0.6f * std::sin(2.0f * pi * 40.0f * t) + 0.4f * std::sin(2.0f * pi * 3.0f * t);
    }

    // A sine of one and a half cycles, 48 values, cut by a clip
    std::vector<float> cut(48);
    for (size_t i = 0; i < cut.size(); ++i)
        cut[i] = 0.8f * std::sin(2.0f * pi * 3.0f * static_cast<float>(i) / 47.0f);

    // Two values: a straight line from low left to high right
    const float straight[2] = {0.2f, 0.8f};

    return renderPicture(640, 480, daw::studioTheme(true), [&](ui::Context &context) {
        paintOverlay(context, [&](ui::DrawList &list, const ui::Theme &theme, const ui::Rect &) {
            // The sine: a Line in the accent, over a Fill of the same values to the middle
            ui::Plot fill = theme.accentPlot(box(20, 30, 600, 120), ui::PlotMode::Fill, 0.15f);
            fill.values = sine.data();
            fill.count = static_cast<uint32_t>(sine.size());
            fill.low = -1.0f;
            fill.high = 1.0f;
            fill.baseline = 0.0f;
            list.add(fill);
            ui::Plot sineLine = theme.accentPlot(box(20, 30, 600, 120), ui::PlotMode::Line);
            sineLine.values = sine.data();
            sineLine.count = static_cast<uint32_t>(sine.size());
            sineLine.width = 2.0f;
            list.add(sineLine);

            // The envelope: a Fill down to the bottom, faint, under an ink Line
            ui::Plot envFill = theme.inkPlot(box(20, 170, 290, 90), ui::PlotMode::Fill, ui::TextTone::Secondary, 0.2f);
            envFill.values = envelope.data();
            envFill.count = static_cast<uint32_t>(envelope.size());
            envFill.low = 0.0f;
            envFill.high = 1.0f;
            envFill.baseline = 0.0f; // the bottom
            list.add(envFill);
            ui::Plot envLine = theme.inkPlot(box(20, 170, 290, 90), ui::PlotMode::Line, ui::TextTone::Primary);
            envLine.values = envelope.data();
            envLine.count = static_cast<uint32_t>(envelope.size());
            envLine.low = 0.0f;
            envLine.high = 1.0f;
            list.add(envLine);

            // The waveform: a Range of 300 columns, bars three quarters of their width
            ui::Plot wave = theme.inkPlot(box(20, 290, 600, 100), ui::PlotMode::Range, ui::TextTone::Secondary);
            wave.values = waveform.data();
            wave.count = 300;
            wave.barFraction = 0.75f;
            list.add(wave);

            // 4096 values in 300 px
            ui::Plot denseLine = theme.accentPlot(box(330, 170, 300, 90), ui::PlotMode::Line);
            denseLine.values = dense.data();
            denseLine.count = static_cast<uint32_t>(dense.size());
            denseLine.width = 1.5f;
            list.add(denseLine);

            // A Line cut short by its clip
            list.pushClip(box(20, 400, 200, 60));
            ui::Plot clipped = theme.accentPlot(box(20, 400, 400, 60), ui::PlotMode::Line);
            clipped.values = cut.data();
            clipped.count = static_cast<uint32_t>(cut.size());
            list.add(clipped);
            list.popClip();

            // Two values: a straight line
            ui::Plot straightLine = theme.inkPlot(box(440, 400, 180, 60), ui::PlotMode::Line, ui::TextTone::Primary);
            straightLine.values = straight;
            straightLine.count = 2;
            straightLine.low = 0.0f;
            straightLine.high = 1.0f;
            list.add(straightLine);
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

// The arrangement over the demo project; with `after`, one more frame is run after the picture
Picture renderArrangement(ui::FrameStats *after)
{
    daw::ProjectStore store(daw::demoProject()); // outlives the context
    return renderPicture(1200, 520, daw::studioTheme(true), [&store](ui::Context &context) {
        stretch(context.root().add<daw::ArrangementView>(store, quietTransport()));
    }, 0, nullptr, after);
}

Picture sceneArrangement()
{
    return renderArrangement(nullptr);
}

// One track's sound for the waveforms scene: 30 seconds of stereo noise, a decaying burst every half
// second, its decay and level set by the track so the eight waveforms differ
std::shared_ptr<audio::AudioData> makeWaveform(int track)
{
    const double rate = 48000.0;
    const uint32_t channels = 2;
    const size_t frames = 30 * 48000;
    const size_t period = 24000; // a burst every half second
    const float decay = 6.0f + 3.0f * static_cast<float>(track);     // per second
    const float level = 0.35f + 0.07f * static_cast<float>(track);

    std::vector<float> samples(frames * channels);
    uint32_t state = 0x9E3779B9u * static_cast<uint32_t>(track + 1); // seeds the noise per track
    for (size_t i = 0; i < frames; ++i) {
        const float t = static_cast<float>(i % period) / static_cast<float>(rate);
        const float envelope = std::exp(-decay * t) * level;
        for (uint32_t c = 0; c < channels; ++c) {
            state = state * 1664525u + 1013904223u; // a linear congruential generator
            const float noise = static_cast<float>(state >> 8) / 8388608.0f - 1.0f; // -1 to 1
            samples[i * channels + c] = noise * envelope;
        }
    }
    return audio::makeAudioData("waveform-" + std::to_string(track + 1), rate, channels, std::move(samples));
}

// Eight audio tracks, each with one long clip of its sound from beat 0, faded in and out
daw::Project waveformProject()
{
    daw::Project project;
    project.name = "Waveforms";
    for (int t = 0; t < 8; ++t) {
        daw::Clip clip("Take " + std::to_string(t + 1), 0.0, 48.0);
        clip.audioFile = "waveform-" + std::to_string(t + 1);
        clip.fadeIn = 0.5;
        clip.fadeOut = 1.0;

        daw::Track track;
        track.name = "Audio " + std::to_string(t + 1);
        track.clips.push_back(clip);
        project.tracks.push_back(track);
    }
    return project;
}

// The arrangement over the eight long clips; with timedFrames, the settled view is timed (see renderPicture)
Picture renderWaveforms(int timedFrames, Timing *timing, ui::FrameStats *after = nullptr)
{
    // The sounds outlive the scene: the hooks hand out pointers into them
    std::vector<std::shared_ptr<audio::AudioData>> sounds;
    for (int t = 0; t < 8; ++t)
        sounds.push_back(makeWaveform(t));

    daw::TransportHooks hooks = quietTransport();
    hooks.audioFor = [&sounds](int track, int) -> const audio::AudioData * {
        return track >= 0 && track < static_cast<int>(sounds.size()) ? sounds[track].get() : nullptr;
    };
    daw::ProjectStore store(waveformProject()); // outlives the context
    return renderPicture(1200, 520, daw::studioTheme(true), [&store, &hooks](ui::Context &context) {
        stretch(context.root().add<daw::ArrangementView>(store, hooks));
    }, timedFrames, timing, after);
}

Picture sceneWaveforms()
{
    return renderWaveforms(0, nullptr);
}

// The piano roll on the demo project; with `after`, one more frame is run after the picture
Picture renderPianoRoll(ui::FrameStats *after)
{
    daw::ProjectStore store(daw::demoProject());
    selectFirstInstrumentClip(store);
    return renderPicture(1200, 520, daw::studioTheme(true), [&store](ui::Context &context) {
        stretch(context.root().add<daw::PianoRollView>(store, quietPianoRoll()));
    }, 0, nullptr, after);
}

Picture scenePianoRoll()
{
    return renderPianoRoll(nullptr);
}

// The step sequencer on a drum clip; with `after`, one more frame is run after the picture
Picture renderSteps(ui::FrameStats *after)
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
    }, 0, nullptr, after);
}

Picture sceneSteps()
{
    return renderSteps(nullptr);
}

// A long list in a scroll view: 2000 rows, of which only the ones on screen should draw
Picture sceneScroll()
{
    return renderPicture(600, 400, daw::studioTheme(true), [](ui::Context &context) {
        ui::ScrollView &scroll = context.root().add<ui::ScrollView>();
        stretch(scroll);
        for (int row = 0; row < 2000; ++row)
            scroll.content()
                .add<ui::ListItem>("Row " + std::to_string(row), [] {})
                .setSubtitle("Subtitle of row " + std::to_string(row));
    });
}

// The synthesizer's panel; with timedFrames, the settled panel is timed (see renderPicture)
Picture renderSynth(int timedFrames, Timing *timing, ui::FrameStats *after = nullptr)
{
    audio::DeviceSetting setting = audio::SynthDevice::setting("Init"); // outlives the context
    daw::DeviceAccess access;
    access.setting = [&setting] { return &setting; };
    access.edit = [&setting] { return &setting; };
    access.device = [] { return static_cast<audio::Device *>(nullptr); };
    return renderPicture(1180, 860, daw::studioTheme(true),
                         [&access](ui::Context &context) {
                             stretch(context.root().add<daw::SynthView>(context, access));
                         },
                         timedFrames, timing, after);
}

Picture sceneSynth()
{
    return renderSynth(0, nullptr);
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

// Packed colours are rounded to 1/255, so a checked component may be that far off (and a little more)
const float kColorTolerance = 1.0f / 255.0f + 0.001f;

bool nearValue(float a, float b)
{
    return std::fabs(a - b) <= kColorTolerance;
}

// The red, green and blue of a packed colour are near the given ones
bool nearRgb(uint32_t packed, float r, float g, float b)
{
    const ui::Color c = ui::unpackColor(packed);
    return nearValue(c.r, r) && nearValue(c.g, g) && nearValue(c.b, b);
}

// A packed colour's components, as text
std::string describeColor(uint32_t packed)
{
    const ui::Color c = ui::unpackColor(packed);
    const float values[4] = {c.r, c.g, c.b, c.a};
    return describe(values, 4);
}

// One letter a prim: S surface, G glyph, I image, L line, P plot, in drawing order
std::string kindsOf(const ui::DrawList &list)
{
    std::string s;
    for (const ui::Prim &p : list.prims()) {
        switch (static_cast<ui::PrimKind>(p.kind)) {
        case ui::PrimKind::Surface: s += 'S'; break;
        case ui::PrimKind::Glyph: s += 'G'; break;
        case ui::PrimKind::Image: s += 'I'; break;
        case ui::PrimKind::Line: s += 'L'; break;
        case ui::PrimKind::Plot: s += 'P'; break;
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
        s.rect = box(60, 25, 40, 40); // inside the inner clip, so culling keeps it
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
        c.check(near(p.style[3], 2.0f) && nearValue(ui::unpackColor(p.base).a, 0.9f),
                "The form is in style and the opacity in base", describe(p.style, 4) + " / " + describeColor(p.base));
        c.check(nearRgb(p.base, 0.8f, 0.7f, 0.6f), "A surface's colour is in base", describeColor(p.base));
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
        c.check(list.prims().size() == 5 && nearRgb(list.prims()[0].base, 0.1f, 0.2f, 0.3f),
                "Glyphs carry the text's colour in base", describeColor(list.prims()[0].base));
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

    // Culling: a prim whose drawn area misses its clip (or the bounds) is not added
    {
        ui::DrawList list;
        list.setFont(&theme.font());
        ui::Surface flat;
        flat.rect = box(0, 0, 40, 40);
        list.pushClip(box(100, 100, 50, 50));
        list.add(flat); // entirely outside
        flat.rect = box(90, 90, 30, 30);
        list.add(flat); // partly inside
        list.popClip();
        c.check(kindsOf(list) == "S", "A flat surface outside its clip adds nothing, one partly in is added",
                kindsOf(list));
    }
    {
        // A raised surface's drawn area grows by its shadow: 1.4143 * distance + blur + 1 (prim.vert)
        ui::Surface raised;
        raised.rect = box(-24, 20, 20, 20); // right edge at -4: outside, but its margin is 9.24 ...
        raised.elevation = ui::Elevation::Raised;
        raised.distance = 3.0f;
        raised.blur = 4.0f;
        ui::Surface flat = raised;
        flat.elevation = ui::Elevation::Flat;
        ui::DrawList list;
        list.setFont(&theme.font());
        list.pushClip(box(0, 0, 100, 100));
        list.add(raised); // ... so its shadow reaches in and it is added
        list.add(flat);   // a flat one just as far out draws nothing inside
        list.popClip();
        c.check(kindsOf(list) == "S" && list.counts().surfaces == 1,
                "A raised surface just outside its clip is added when its shadow reaches in", kindsOf(list));
    }
    {
        ui::DrawList list;
        list.setFont(&theme.font());
        list.setBounds(box(0, 0, 100, 100));
        ui::Surface s;
        s.rect = box(500, 10, 40, 40);
        list.add(s); // outside the bounds
        s.rect = box(50, 10, 40, 40);
        list.add(s); // inside
        c.check(kindsOf(list) == "S", "Bounds drop a surface outside them and keep one inside", kindsOf(list));
    }
    {
        // A text half outside its clip keeps the glyphs that overlap the clip
        ui::Text sentence = hello;
        sentence.text = "Hello world, cut in half";
        std::vector<ui::GlyphQuad> quads;
        theme.font().layout(sentence.text, sentence.baseline, sentence.size, quads);
        const float middle = (quads.front().x0 + quads.back().x1) / 2.0f;

        ui::DrawList whole;
        whole.setFont(&theme.font());
        whole.add(sentence);
        ui::DrawList cut;
        cut.setFont(&theme.font());
        cut.pushClip(box(0, 0, middle, 100));
        cut.add(sentence);
        cut.popClip();
        const size_t laid = quads.size(), kept = cut.prims().size();
        c.check(whole.prims().size() == laid && kept > 0 && kept < laid,
                "A text half outside its clip keeps only the glyphs that overlap it",
                std::to_string(kept) + " of " + std::to_string(laid));
    }
    {
        ui::DrawList list;
        list.setFont(&theme.font());
        list.pushClip(box(0, 0, 10, 10));
        list.add(ui::ImageDraw{box(50, 50, 8, 8), &picture, 1.0f});
        list.popClip();
        c.check(list.empty() && list.images().empty() && list.counts().images == 0,
                "An image outside its clip adds no prim and no reference");
    }
    {
        // Counts give only what was added: the culled surface and text are not counted
        ui::DrawList list;
        list.setFont(&theme.font());
        list.pushClip(box(0, 0, 200, 100));
        ui::Surface s;
        s.rect = box(400, 0, 40, 40);
        list.add(s); // outside
        s.rect = box(10, 10, 20, 20);
        list.add(s); // inside
        list.add(ui::Text{"Hello", {400.0f, 30.0f}, 13.0f, ui::Color{0.1f, 0.2f, 0.3f, 1.0f}}); // outside
        list.add(ui::Text{"Hello", {20.0f, 30.0f}, 13.0f, ui::Color{0.1f, 0.2f, 0.3f, 1.0f}});  // inside
        list.popClip();
        const ui::DrawList::Counts &n = list.counts();
        c.check(n.surfaces == 1 && n.texts == 1 && n.glyphs == 5 && n.images == 0 && list.prims().size() == 6,
                "Counts give only what was added", kindsOf(list));
    }

    // Lines: a capsule's rect grown by half its width and a pixel, its ends in shape, half the width in style
    {
        ui::Line line;
        line.a = {10.0f, 20.0f};
        line.b = {40.0f, 30.0f};
        line.width = 3.0f;
        line.color = ui::Color{0.2f, 0.4f, 0.6f, 1.0f};
        line.opacity = 0.5f;
        const float lineClip[4] = {5, 5, 90, 90};
        const ui::Prim p = ui::linePrim(line, lineClip);

        c.check(p.kind == static_cast<uint32_t>(ui::PrimKind::Line) && near(p.shape[0], 10) && near(p.shape[1], 20) &&
                    near(p.shape[2], 40) && near(p.shape[3], 30),
                "A line prim has its kind and its ends in shape", describe(p.shape, 4));
        c.check(near(p.style[0], 1.5f), "A line's style holds half its width", describe(p.style, 4));
        c.check(near(p.rect[0], 7.5f) && near(p.rect[1], 17.5f) && near(p.rect[2], 35.0f) && near(p.rect[3], 15.0f),
                "A line's rect covers both ends grown by half the width and a pixel", describe(p.rect, 4));
        c.check(isClip(p.clip, 5, 5, 90, 90), "A line prim carries the clip it was given", describe(p.clip, 4));
        c.check(nearRgb(p.base, 0.2f, 0.4f, 0.6f) && nearValue(ui::unpackColor(p.base).a, 0.5f),
                "A line's colour is in base, with its opacity as alpha", describeColor(p.base));

        line.width = 0.4f; // below a pixel: drawn half a pixel thick
        const ui::Prim thin = ui::linePrim(line, lineClip);
        c.check(near(thin.style[0], 0.5f) && near(thin.rect[0], 8.5f) && near(thin.rect[2], 33.0f),
                "A line narrower than a pixel is 0.5 in style", describe(thin.style, 4));
    }
    {
        ui::DrawList list;
        ui::Line line;
        line.a = {10.0f, 20.0f};
        line.b = {40.0f, 30.0f};
        line.width = 2.0f;
        list.add(line);
        c.check(kindsOf(list) == "L" && list.counts().lines == 1, "A line adds one prim and counts one line",
                kindsOf(list));
    }
    {
        // A line wholly outside its clip is culled
        ui::DrawList list;
        ui::Line line;
        line.a = {200.0f, 50.0f};
        line.b = {300.0f, 50.0f};
        line.width = 2.0f;
        list.pushClip(box(0, 0, 100, 100));
        list.add(line);
        list.popClip();
        c.check(list.empty() && list.counts().lines == 0, "A line outside its clip adds nothing");
    }
    {
        ui::DrawList list;
        const std::vector<ui::Vec2> five = {{0, 0}, {10, 5}, {20, 0}, {30, 5}, {40, 0}};
        ui::Line style;
        style.width = 2.0f;
        style.color = ui::Color{0.2f, 0.4f, 0.6f, 1.0f};
        list.addPolyline(five.data(), five.size(), style);
        c.check(kindsOf(list) == "LLLL" && list.counts().lines == 4, "A polyline of 5 points adds 4 lines",
                kindsOf(list));
        c.check(!list.prims().empty() && nearRgb(list.prims().back().base, 0.2f, 0.4f, 0.6f),
                "A polyline's segments take the style's colour", describeColor(list.prims().back().base));
    }
    {
        ui::DrawList list;
        const ui::Vec2 one[1] = {{5, 5}};
        ui::Line style;
        style.width = 2.0f;
        list.addPolyline(one, 1, style);
        c.check(list.empty() && list.counts().lines == 0, "A polyline of 1 point adds nothing");
    }
    {
        // The accent line is drawn in the theme's accent
        const ui::Color accent = theme.accentColor();
        const ui::Line accentLine = theme.accentLine({0.0f, 0.0f}, {10.0f, 0.0f}, 2.0f);
        const float lineClip[4] = {-1000, -1000, 1000, 1000};
        const ui::Prim p = ui::linePrim(accentLine, lineClip);
        c.check(nearRgb(p.base, accent.r, accent.g, accent.b) && nearValue(ui::unpackColor(p.base).a, 1.0f),
                "accentLine is the theme's accent", describeColor(p.base));
    }

    // Plots: the prim packs the first value, count, low and high in shape; mode, half the width (Line) or
    // the bar fraction (Range) and the baseline in style; the colour with the opacity as alpha
    {
        const float values[4] = {0.0f, 1.0f, -1.0f, 0.5f};
        ui::Plot plot;
        plot.rect = box(10, 20, 100, 40);
        plot.mode = ui::PlotMode::Line;
        plot.values = values;
        plot.count = 4;
        plot.low = -2.0f;
        plot.high = 3.0f;
        plot.width = 4.0f;
        plot.color = ui::Color{0.2f, 0.4f, 0.6f, 1.0f};
        plot.opacity = 0.5f;
        const float plotClip[4] = {5, 5, 200, 200};
        const ui::Prim p = ui::plotPrim(plot, 7, plotClip);

        c.check(p.kind == static_cast<uint32_t>(ui::PrimKind::Plot) && near(p.rect[0], 10) && near(p.rect[1], 20) &&
                    near(p.rect[2], 100) && near(p.rect[3], 40),
                "A plot prim has its kind and rect", describe(p.rect, 4));
        c.check(near(p.shape[0], 7) && near(p.shape[1], 4) && near(p.shape[2], -2) && near(p.shape[3], 3),
                "A plot's shape holds its first value, count, low and high", describe(p.shape, 4));
        c.check(near(p.style[0], 0) && near(p.style[1], 2), "A Line plot's style holds mode 0 and half its width",
                describe(p.style, 4));
        c.check(isClip(p.clip, 5, 5, 200, 200), "A plot prim carries the clip it was given", describe(p.clip, 4));
        c.check(nearRgb(p.base, 0.2f, 0.4f, 0.6f) && nearValue(ui::unpackColor(p.base).a, 0.5f),
                "A plot's colour is in base, with its opacity as alpha", describeColor(p.base));

        plot.mode = ui::PlotMode::Fill;
        plot.baseline = 0.25f;
        const ui::Prim fill = ui::plotPrim(plot, 0, plotClip);
        c.check(near(fill.style[0], 1) && near(fill.style[2], 0.25f), "A Fill plot's style holds mode 1 and its baseline",
                describe(fill.style, 4));

        plot.mode = ui::PlotMode::Range;
        plot.count = 3; // pairs
        plot.barFraction = 0.75f;
        const ui::Prim range = ui::plotPrim(plot, 0, plotClip);
        c.check(near(range.style[0], 2) && near(range.style[1], 0.75f) && near(range.shape[1], 3),
                "A Range plot's style holds mode 2 and its bar fraction, its count in pairs", describe(range.style, 4));
    }
    {
        // add(Plot) copies the values into the plot data and counts one plot
        const float values[4] = {0.25f, 0.5f, 0.75f, 1.0f};
        ui::Plot plot;
        plot.rect = box(10, 10, 100, 40);
        plot.values = values;
        plot.count = 4;
        ui::DrawList list;
        list.setFont(&theme.font());
        list.add(plot);
        bool copied = list.plotData().size() == 4;
        for (size_t i = 0; copied && i < 4; ++i)
            copied = near(list.plotData()[i], values[i]);
        c.check(kindsOf(list) == "P" && list.counts().plots == 1 && copied,
                "A Line plot adds one prim, counts one plot and copies its values", kindsOf(list));

        ui::DrawList ranged;
        ranged.setFont(&theme.font());
        plot.mode = ui::PlotMode::Range;
        plot.count = 3; // three pairs: six floats
        ranged.add(plot);
        c.check(ranged.plotData().size() == 6 && kindsOf(ranged) == "P",
                "A Range plot copies 2 x count values", std::to_string(ranged.plotData().size()));
    }
    {
        // Nothing for a null values pointer, nor for a one-value Line
        ui::DrawList list;
        list.setFont(&theme.font());
        ui::Plot plot;
        plot.rect = box(10, 10, 100, 40);
        plot.count = 4;
        list.add(plot); // values null
        const float one[1] = {0.5f};
        plot.values = one;
        plot.count = 1;
        plot.mode = ui::PlotMode::Line;
        list.add(plot);
        c.check(list.empty() && list.plotData().empty() && list.counts().plots == 0,
                "A plot with null values, or one Line value, adds nothing");
    }
    {
        // A plot outside its clip adds no prim and no data
        const float values[3] = {0.1f, 0.2f, 0.3f};
        ui::DrawList list;
        list.setFont(&theme.font());
        ui::Plot plot;
        plot.rect = box(200, 200, 100, 40);
        plot.values = values;
        plot.count = 3;
        list.pushClip(box(0, 0, 50, 50));
        list.add(plot);
        list.popClip();
        c.check(list.empty() && list.plotData().empty() && list.counts().plots == 0,
                "A plot outside its clip adds no prim and no data");
    }
    {
        // Two plots in a row: the second's first value follows the first's values
        const float first[4] = {0.0f, 0.1f, 0.2f, 0.3f};
        const float second[3] = {0.9f, 0.8f, 0.7f};
        ui::DrawList list;
        list.setFont(&theme.font());
        ui::Plot plot;
        plot.rect = box(10, 10, 100, 40);
        plot.values = first;
        plot.count = 4;
        list.add(plot);
        plot.values = second;
        plot.count = 3;
        list.add(plot);
        c.check(kindsOf(list) == "PP" && list.prims().size() == 2 && near(list.prims()[1].shape[0], 4) &&
                    list.plotData().size() == 7 && list.counts().plots == 2,
                "The second plot starts at the first's count in the plot data",
                list.prims().size() == 2 ? describe(list.prims()[1].shape, 4) : kindsOf(list));
    }
    {
        // The fingerprint sees one changed plot value; clear empties the plot data
        const float a[4] = {0.0f, 0.5f, 1.0f, 0.5f};
        const float b[4] = {0.0f, 0.5f, 0.9f, 0.5f};
        auto make = [&](const float *values) {
            ui::DrawList list;
            list.setFont(&theme.font());
            ui::Plot plot;
            plot.rect = box(10, 10, 100, 40);
            plot.values = values;
            plot.count = 4;
            list.add(plot);
            return ui::fingerprint(list);
        };
        c.check(make(a) == make(a) && make(a) != make(b), "The fingerprint changes when one plot value changes");

        ui::DrawList list;
        list.setFont(&theme.font());
        ui::Plot plot;
        plot.rect = box(10, 10, 100, 40);
        plot.values = a;
        plot.count = 4;
        list.add(plot);
        list.clear();
        c.check(list.plotData().empty() && list.empty() && list.counts().plots == 0,
                "clear empties the plot data and the plot count");
    }
    std::printf("\n");
}

// ---- draw sections and draw caches (no window) ----

// Whether two lists hold the same prims, byte for byte
bool samePrims(const ui::DrawList &a, const ui::DrawList &b)
{
    if (a.prims().size() != b.prims().size())
        return false;
    return a.prims().empty() ||
           std::memcmp(a.prims().data(), b.prims().data(), a.prims().size() * sizeof(ui::Prim)) == 0;
}

// An empty list with the theme's font, as a frame's list has
ui::DrawList fontedList(const ui::Theme &theme)
{
    ui::DrawList list;
    list.setFont(&theme.font());
    return list;
}

const float kBeforeValues[3] = {0.2f, 0.5f, 0.8f};        // the plot drawn before a section
const float kSectionValues[4] = {0.1f, 0.6f, 0.3f, 0.9f}; // the plot drawn in a section

// What comes before a section in each list, a plot included, so the section's offsets are not zero
void addBefore(ui::DrawList &list, const ui::Theme &theme)
{
    list.add(theme.surface(box(4, 4, 40, 40), ui::Shape::Rounded, ui::Elevation::Flat));
    ui::Plot plot = theme.inkPlot(box(60, 4, 80, 40), ui::PlotMode::Line, ui::TextTone::Secondary);
    plot.values = kBeforeValues;
    plot.count = 3;
    plot.low = 0.0f;
    plot.high = 1.0f;
    list.add(plot);
}

// The content a section paints: a surface, a text, a Line and a Plot
void paintSection(ui::DrawList &list, const ui::Theme &theme)
{
    list.add(theme.surface(box(30, 30, 60, 40), ui::Shape::Rounded, ui::Elevation::Raised));
    list.add(theme.text("Section", box(30, 80, 120, 24), ui::TextStyle::Body, ui::TextTone::Primary, ui::TextAlign::Start));
    list.add(theme.inkLine({30.0f, 120.0f}, {150.0f, 112.0f}, 2.0f, ui::TextTone::Secondary, 1.0f));
    ui::Plot plot = theme.accentPlot(box(30, 130, 120, 40), ui::PlotMode::Line);
    plot.values = kSectionValues;
    plot.count = 4;
    plot.low = 0.0f;
    plot.high = 1.0f;
    list.add(plot);
}

// What a cached view paints: a raised surface and a text
void paintCached(ui::DrawList &list, const ui::Theme &theme)
{
    list.add(theme.surface(box(10, 10, 50, 30), ui::Shape::Rounded, ui::Elevation::Raised));
    list.add(theme.text("Cache", box(10, 50, 80, 24), ui::TextStyle::Body, ui::TextTone::Primary, ui::TextAlign::Start));
}

// The DrawCache counts: a miss paints, the same key again appends, and a change of key, reuse, invalidate,
// rect or clip paints again. Only without VIBIN_VERIFY_CACHE, which paints every draw.
void cacheChecks(Checker &c, const ui::Theme &theme)
{
    ui::DrawCache cache("sections-check");
    (void)ui::DrawCache::takeTotals(); // forget what earlier draws counted
    int paints = 0;
    const ui::Rect wide = box(0, 0, 200, 100);
    const ui::Rect tall = box(0, 0, 300, 100);
    auto draw = [&](ui::DrawList &list, const ui::Rect &rect, uint64_t key, bool reuse) {
        cache.draw(list, theme, rect, key, [&] { ++paints; paintCached(list, theme); }, reuse);
    };

    ui::DrawList first = fontedList(theme), second = fontedList(theme);
    draw(first, wide, 7, true);
    draw(second, wide, 7, true);
    c.check(paints == 1 && cache.hits() == 1 && cache.misses() == 1,
            "The same key paints once: a miss, then a hit",
            "paints " + std::to_string(paints) + ", hits " + std::to_string(cache.hits()) + ", misses " +
                std::to_string(cache.misses()));
    c.check(!first.empty() && samePrims(first, second), "The cached draw gives the same prims as painting");
    const ui::DrawCache::Totals totals = ui::DrawCache::takeTotals();
    const ui::DrawCache::Totals rest = ui::DrawCache::takeTotals();
    c.check(totals.hits == 1 && totals.misses == 1 && rest.hits == 0 && rest.misses == 0,
            "takeTotals reports the hits and misses, then zero",
            std::to_string(totals.hits) + " hits, " + std::to_string(totals.misses) + " misses");

    ui::DrawList third = fontedList(theme);
    draw(third, wide, 8, true);
    c.check(paints == 2, "A new key paints again", "paints " + std::to_string(paints));

    ui::DrawList r1 = fontedList(theme), r2 = fontedList(theme);
    draw(r1, wide, 8, false);
    draw(r2, wide, 8, false);
    c.check(paints == 4 && cache.hits() == 1, "With reuse false every draw paints", "paints " + std::to_string(paints));

    cache.invalidate();
    ui::DrawList i1 = fontedList(theme), i2 = fontedList(theme);
    draw(i1, wide, 8, true);
    draw(i2, wide, 8, true);
    c.check(paints == 5 && samePrims(i1, i2), "After invalidate the next draw paints, and the one after is a hit",
            "paints " + std::to_string(paints));

    ui::DrawList t1 = fontedList(theme);
    draw(t1, tall, 8, true);
    c.check(paints == 6, "A different rect paints again", "paints " + std::to_string(paints));

    ui::DrawList c1 = fontedList(theme);
    c1.pushClip(box(0, 0, 50, 50));
    draw(c1, tall, 8, true);
    c.check(paints == 7, "A different clip around it paints again", "paints " + std::to_string(paints));
    ui::DrawList c2 = fontedList(theme);
    c2.pushClip(box(0, 0, 50, 50));
    draw(c2, tall, 8, true);
    c.check(paints == 7 && !c1.empty() && samePrims(c1, c2), "The same clip again is a hit with the same prims",
            "paints " + std::to_string(paints));
}

// With VIBIN_VERIFY_CACHE every draw paints, so a matched key's paint is compared with the cached section
void verifierChecks(Checker &c, const ui::Theme &theme)
{
    ui::DrawCache verifier("verifier-check");
    int paints = 0;
    const ui::Rect rect = box(0, 0, 200, 100);
    ui::DrawList x = fontedList(theme), y = fontedList(theme);
    verifier.draw(x, theme, rect, 3, [&] { ++paints; paintCached(x, theme); });
    verifier.draw(y, theme, rect, 3, [&] { ++paints; paintCached(y, theme); });
    c.check(paints == 2 && !x.empty() && samePrims(x, y), "With the verifier every draw paints, and the same content matches",
            "paints " + std::to_string(paints));
}

void sectionChecks(Checker &c)
{
    std::printf("Draw sections and draw caches: appending, nesting, hashes, no window\n\n");
    const ui::Theme theme = ui::Theme::light(); // outlives the lists, which lay their text out with its font

    // A section painted, then appended into a second list, gives the prims and plot data of painting it directly
    ui::DrawSection section;
    {
        ui::DrawList direct = fontedList(theme);
        addBefore(direct, theme);
        const size_t before = direct.prims().size();
        paintSection(direct, theme);

        ui::DrawList sectioned = fontedList(theme);
        addBefore(sectioned, theme);
        sectioned.beginSection();
        paintSection(sectioned, theme);
        sectioned.endSection(section);

        ui::DrawList appended = fontedList(theme);
        addBefore(appended, theme);
        appended.appendSection(section);

        c.check(samePrims(direct, appended) && direct.plotData() == appended.plotData(),
                "An appended section gives the prims and plot data of painting it directly",
                kindsOf(appended));
        c.check(samePrims(sectioned, appended) && sectioned.hash() == appended.hash(),
                "A painted section and an appended one leave the same prims and hash");
        c.check(section.prims.size() == direct.prims().size() - before && section.data.size() == 4,
                "The section holds its own prims and plot values", std::to_string(section.prims.size()) + " prims");

        // The appended plot's first value points at its values in the new list, after the first plot's
        const ui::Prim &plot = appended.prims().back();
        const size_t first = static_cast<size_t>(std::lround(plot.shape[0]));
        bool values = plot.kind == static_cast<uint32_t>(ui::PrimKind::Plot) && first + 4 <= appended.plotData().size();
        for (size_t i = 0; values && i < 4; ++i)
            values = near(appended.plotData()[first + i], kSectionValues[i]);
        c.check(values && first == 3, "An appended plot reads its own values in the new list",
                "first value " + std::to_string(first));

        // The fingerprints of the painted and the appended lists agree, and equal content agrees
        ui::DrawList again = fontedList(theme);
        addBefore(again, theme);
        paintSection(again, theme);
        c.check(ui::fingerprint(direct) == ui::fingerprint(again), "Two lists with the same content have equal fingerprints");
        c.check(ui::fingerprint(sectioned) == ui::fingerprint(appended),
                "A section's hash folded in makes the painted and appended fingerprints equal");
    }

    // endSection reports an image in the section, and none without one
    {
        ui::ImageData image;
        makeImage(image);
        ui::DrawList withImage = fontedList(theme);
        withImage.beginSection();
        withImage.add(theme.surface(box(0, 0, 40, 40), ui::Shape::Rounded, ui::Elevation::Flat));
        withImage.add(ui::ImageDraw{box(0, 0, 8, 8), &image, 1.0f});
        ui::DrawSection imaged;
        withImage.endSection(imaged);

        ui::DrawList plain = fontedList(theme);
        plain.beginSection();
        paintSection(plain, theme);
        ui::DrawSection unimaged;
        plain.endSection(unimaged);
        c.check(imaged.hasImages && !unimaged.hasImages, "endSection reports an image in its section and none without one");
    }

    // Nested sections: appending the outer one reproduces both
    {
        ui::DrawList nested = fontedList(theme);
        addBefore(nested, theme);
        nested.beginSection(); // the outer
        nested.add(theme.surface(box(30, 30, 60, 40), ui::Shape::Rounded, ui::Elevation::Raised));
        nested.beginSection(); // the inner
        nested.add(theme.text("Inner", box(30, 80, 120, 24), ui::TextStyle::Body, ui::TextTone::Primary, ui::TextAlign::Start));
        ui::DrawSection inner;
        nested.endSection(inner);
        nested.add(theme.inkLine({30.0f, 120.0f}, {150.0f, 112.0f}, 2.0f, ui::TextTone::Secondary, 1.0f));
        ui::DrawSection outer;
        nested.endSection(outer);

        ui::DrawList direct = fontedList(theme);
        addBefore(direct, theme);
        direct.add(theme.surface(box(30, 30, 60, 40), ui::Shape::Rounded, ui::Elevation::Raised));
        direct.add(theme.text("Inner", box(30, 80, 120, 24), ui::TextStyle::Body, ui::TextTone::Primary, ui::TextAlign::Start));
        direct.add(theme.inkLine({30.0f, 120.0f}, {150.0f, 112.0f}, 2.0f, ui::TextTone::Secondary, 1.0f));

        ui::DrawList appended = fontedList(theme);
        addBefore(appended, theme);
        appended.appendSection(outer);
        c.check(samePrims(direct, appended) && nested.hash() == appended.hash(),
                "An outer section appended reproduces both it and its inner section");
        c.check(!inner.prims.empty() && inner.prims.size() < outer.prims.size(),
                "The inner section is part of the outer one", std::to_string(inner.prims.size()) + " of " +
                                                                  std::to_string(outer.prims.size()) + " prims");
    }

    // The draw cache's counts, or the verifier's check when VIBIN_VERIFY_CACHE is set
    if (ui::DrawCache::verifying()) {
        std::printf("SKIP  %-44s %s\n", "Draw cache counts", "VIBIN_VERIFY_CACHE is set, so every draw paints");
        verifierChecks(c, theme);
    } else {
        cacheChecks(c, theme);
        std::printf("SKIP  %-44s %s\n", "Draw cache verifier",
                    "VIBIN_VERIFY_CACHE is not set; the verifier is checked by running the app");
    }
    std::printf("\n");
}

// A DAW view's scene, rendered again with one more frame after it has settled (see settledCacheChecks)
struct CacheScene {
    const char *name;
    Picture (*render)(ui::FrameStats *after);
};

const CacheScene kCacheScenes[] = {
    {"arrangement", [](ui::FrameStats *after) { return renderArrangement(after); }},
    {"waveforms", [](ui::FrameStats *after) { return renderWaveforms(0, nullptr, after); }},
    {"pianoroll", renderPianoRoll},
    {"steps", renderSteps},
    {"synth", [](ui::FrameStats *after) { return renderSynth(0, nullptr, after); }},
};

// A scene settled, then one more frame: its views' draw caches serve every draw (hits, no misses) and the layout is skipped
void settledCacheChecks(Checker &c, const CacheScene &scene)
{
    const std::string title = std::string(scene.name) + ": a settled frame draws from its caches";
    if (ui::DrawCache::verifying()) {
        std::printf("SKIP  %-44s %s\n", title.c_str(), "VIBIN_VERIFY_CACHE is set, so every draw paints");
        return;
    }
    ui::FrameStats after;
    try {
        scene.render(&after);
    } catch (const std::exception &e) {
        c.check(false, title, std::string("render failed: ") + e.what());
        return;
    }
    const bool settled = after.cacheHits >= 1 && after.cacheMisses == 0 && after.layoutSkipped;
    c.check(settled, title,
            std::to_string(after.cacheHits) + " hits, " + std::to_string(after.cacheMisses) + " misses, layout " +
                (after.layoutSkipped ? "skipped" : "run"));
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
    {"lines", sceneLines},
    {"plots", scenePlots},
    {"controls", sceneControls},
    {"arrangement", sceneArrangement},
    {"waveforms", sceneWaveforms},
    {"pianoroll", scenePianoRoll},
    {"steps", sceneSteps},
    {"scroll", sceneScroll},
    {"synth", sceneSynth},
};

// Times a scene over kTimedFrames settled frames when it is the one asked for (or all are run) and
// prints an INFO line; returns 1 when the timing throws, else 0
int timeScene(const char *name, const std::string &only, const std::function<void(int, Timing *)> &render)
{
    if (!only.empty() && only != name)
        return 0;
    try {
        Timing timing;
        render(kTimedFrames, &timing);
        std::printf("INFO render %s x%d: build %.2f ms, submit %.2f ms, %u prims, %zu KB\n", name, timing.frames,
                    timing.buildMs, timing.submitMs, timing.prims, timing.uploadBytes / 1024);
        return 0;
    } catch (const std::exception &e) {
        std::printf("FAIL  %s timing failed: %s\n", name, e.what());
        return 1;
    }
}

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
    std::printf("INFO render %s: build %.2f ms, %u surfaces, %u texts, %u glyphs, %u prims, %u draws, %zu KB, submit %.2f ms, %zu plot values\n",
                name.c_str(), s.buildMs, s.surfaces, s.texts, s.glyphs, s.surfaces + s.glyphs + s.images + s.lines + s.plots,
                s.draws, s.uploadBytes / 1024, s.submitMs, s.plotValues);
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
    sectionChecks(prims);
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

    // The views' draw caches, on a frame after each scene has settled (a second render of the scene)
    Checker caches;
    bool cacheHeading = false;
    for (const CacheScene &scene : kCacheScenes) {
        if (!only.empty() && only != scene.name)
            continue;
        if (!cacheHeading) {
            std::printf("Settled frames: their scenes draw from their draw caches\n\n");
            cacheHeading = true;
        }
        settledCacheChecks(caches, scene);
    }
    if (cacheHeading)
        std::printf("\n");
    failures += caches.failures;

    for (const auto &entry : rendered)
        printInfo(entry.first, entry.second);

    // The heaviest scenes, timed on their settled frames
    failures += timeScene("synth", only, [](int frames, Timing *timing) { renderSynth(frames, timing); });
    failures += timeScene("waveforms", only, [](int frames, Timing *timing) { renderWaveforms(frames, timing); });
    return failures;
}

}
