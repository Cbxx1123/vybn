// Scenes, each described in RenderSelfTest.h: shapes, shapes-dark, text, clips, image, lines, plots,
// controls, arrangement, waveforms, pianoroll, steps, scroll (a 2000-row ScrollView, to show culling at work)
// and synth. The heavy scenes (synth, waveforms, scroll, controls, arrangement, pianoroll, steps) are also timed
// over 50 settled frames (INFO lines after the scenes).
#include "RenderSelfTest.h"
#include "Harness.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <chrono>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>
#include <thread>

#include "audio/AudioAnalysis.h"
#include "audio/AudioFile.h"
#include "audio/AudioSystem.h"
#include "audio/Effects.h"
#include "audio/Synth.h"
#include "audio/DeviceChain.h"
#include "audio/PluginEffect.h"
#include "plugin/Manager.h"
#include "plugin/Slot.h"
#include "daw/app/Appearance.h"
#include "daw/views/Arrangement.h"
#include "daw/views/AudioEditor.h"
#include "daw/views/Browser.h"
#include "daw/library/DeviceCatalogue.h"
#include "daw/library/Online.h"
#include "daw/views/DeviceControls.h"
#include "daw/views/DeviceView.h"
#include "daw/views/DevicesPanel.h"
#include "daw/model/FoldState.h"
#include "daw/model/History.h"
#include "daw/model/Warp.h"
#include "daw/library/Library.h"
#include "daw/views/Mixer.h"
#include "daw/views/PianoRoll.h"
#include "daw/model/Project.h"
#include "daw/model/ProjectStore.h"
#include "daw/model/Takes.h"
#include "daw/model/Routing.h"
#include "daw/app/Settings.h"
#include "daw/views/StepSequencer.h"
#include "daw/views/SynthView.h"
#include "engine/Renderer.h"
#include "engine/Window.h"
#include "env/Environment.h"
#include "env/Scheduler.h"
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

// stb's PNG reader: declarations only, the implementation is compiled in src/daw/library/Archive.cpp
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
    return fs::path(std::string(VYBN_TEST_DIR) + "/render/" + scene + ".png");
}

fs::path outputFolder()
{
    return fs::temp_directory_path() / "vybn-render";
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
    double cacheHits = 0.0;
    double cacheMisses = 0.0;
    double layoutSkipped = 0.0; // the share of frames whose layout was skipped, 0 to 1
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
    engine::Window window(width, height, "VYBN render test", false);
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
            double hitsSum = 0.0, missesSum = 0.0, skippedFrames = 0.0;
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
                hitsSum += s.cacheHits;
                missesSum += s.cacheMisses;
                skippedFrames += s.layoutSkipped ? 1.0 : 0.0;
                timing->prims = s.surfaces + s.glyphs + s.images;
            }
            timing->frames = timedFrames;
            timing->buildMs = buildSum / timedFrames;
            timing->submitMs = submitSum / timedFrames;
            timing->uploadBytes = bytesSum / static_cast<size_t>(timedFrames);
            timing->cacheHits = hitsSum / timedFrames;
            timing->cacheMisses = missesSum / timedFrames;
            timing->layoutSkipped = skippedFrames / timedFrames;
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

Picture renderControls(int timedFrames, Timing *timing)
{
    return renderPicture(900, 600, daw::studioTheme(true), buildControls, timedFrames, timing);
}

Picture sceneControls()
{
    return renderControls(0, nullptr);
}

// The arrangement over the demo project; with timedFrames, the settled view is timed (see renderPicture),
// and with `after`, one more frame is run after the picture
Picture renderArrangement(int timedFrames, Timing *timing, ui::FrameStats *after = nullptr)
{
    daw::ProjectStore store(daw::demoProject()); // outlives the context
    return renderPicture(1200, 520, daw::studioTheme(true), [&store](ui::Context &context) {
        auto &view = context.root().add<daw::ArrangementView>(store, quietTransport());
        view.showTiming(false); // the tempo lanes have their own scene
        stretch(view);
    }, timedFrames, timing, after);
}

Picture sceneArrangement()
{
    return renderArrangement(0, nullptr);
}

// Editing automation in the arrangement, by input handed to the view (laid out by a real context, so
// its geometry is the app's): a click adds a point (snapped to the beat), a drag moves it, a
// double-click and a right-click remove points, a lane's x removes it, and the add strip opens the
// menu. Positions come from the view's layout: the header column (268) and a gap (10) before the
// lanes, the ruler (30) above, a track's row (76) and then its lanes (56 each), 22 pixels a beat.
void automationEditChecks(Checker &c)
{
    engine::Window window(1200, 520, "VYBN automation test", false);
    window.setPosition(-32000, -32000);
    engine::Renderer renderer(window, false, true);
    const ui::Theme theme = daw::studioTheme(true);
    daw::Project project = daw::demoProject();
    project.tracks[0].automation.push_back({audio::kVolumeParam, {}});
    daw::ProjectStore store(project);
    int menuTrack = -1;
    daw::TransportHooks hooks = quietTransport();
    hooks.automationMenu = [&menuTrack](int track, ui::Vec2) { menuTrack = track; };
    ui::Context context(window, renderer, theme);
    auto &view = context.root().add<daw::ArrangementView>(store, hooks);
    view.showTiming(false); // the tempo lanes have their own scene
    stretch(view);
    view.showAutomation(0, true);
    for (int i = 0; i < 4; ++i) {
        context.update();
        context.buildFrame();
    }

    const ui::Rect r = view.rect();
    const float wellX = r.x + 278.0f, laneTop = r.y + 30.0f + 76.0f;
    auto beatX = [wellX](double beat) { return wellX + static_cast<float>(beat) * 22.0f; };
    auto laneY = [laneTop](float position) { return laneTop + 7.0f + (1.0f - position) * (56.0f - 14.0f); };
    double now = 10.0;
    auto send = [&](ui::Vec2 at, bool down, bool pressed, bool secondary = false) {
        ui::Input in;
        in.mouse = at;
        in.down = down;
        in.pressed = pressed;
        in.secondaryPressed = secondary;
        in.time = now;
        view.handleInput(in);
        view.arrange(view.rect(), theme);
        now += 0.5; // presses half a second apart aren't double-clicks
    };
    auto points = [&store]() -> std::vector<audio::AutomationPoint> {
        const audio::AutomationLane *lane = audio::findAutomation(store.project().tracks[0].automation, audio::kVolumeParam);
        return lane ? lane->points : std::vector<audio::AutomationPoint>{};
    };

    // A click at beat 4.3, halfway up: a point at beat 4 (snapped), halfway
    send({beatX(4.3), laneY(0.5f)}, true, true);
    send({beatX(4.3), laneY(0.5f)}, false, false);
    std::vector<audio::AutomationPoint> p = points();
    c.check(p.size() == 1 && p[0].beat == 4.0 && std::fabs(p[0].value - 0.5f) < 0.03f, "Automation lane: a click adds a point, snapped to the beat",
            p.empty() ? "no point" : std::to_string(p[0].beat) + " @ " + std::to_string(p[0].value));

    // Dragging it to beat 8, near the top; Shift lets it off the grid
    send({beatX(4.0), laneY(0.5f)}, true, true);
    send({beatX(8.2), laneY(0.9f)}, true, false);
    send({beatX(8.2), laneY(0.9f)}, false, false);
    p = points();
    c.check(p.size() == 1 && p[0].beat == 8.0 && std::fabs(p[0].value - 0.9f) < 0.03f, "Automation lane: dragging a point moves it",
            p.empty() ? "no point" : std::to_string(p[0].beat) + " @ " + std::to_string(p[0].value));

    // A second point at beat 12; then the first can't be dragged past it
    send({beatX(12.0), laneY(0.2f)}, true, true);
    send({beatX(12.0), laneY(0.2f)}, false, false);
    send({beatX(8.0), laneY(0.9f)}, true, true);
    send({beatX(20.0), laneY(0.9f)}, true, false);
    send({beatX(20.0), laneY(0.9f)}, false, false);
    p = points();
    c.check(p.size() == 2 && p[0].beat == 12.0 && p[1].beat == 12.0, "Automation lane: a point stops at its neighbour",
            std::to_string(p.size()) + " points");

    // A double-click on a point (the low one) removes it; a right-click removes the other
    const double first = now;
    send({beatX(12.0), laneY(0.2f)}, true, true);
    send({beatX(12.0), laneY(0.2f)}, false, false);
    now = first + 0.2;
    send({beatX(12.0), laneY(0.2f)}, true, true);
    send({beatX(12.0), laneY(0.2f)}, false, false);
    const size_t afterDouble = points().size();
    send({beatX(12.0), laneY(0.9f)}, false, false, true);
    c.check(afterDouble == 1 && points().empty(), "Automation lane: a double-click or a right-click removes a point",
            std::to_string(afterDouble) + " after the double-click, " + std::to_string(points().size()) + " after the right-click");

    // The add strip under the lanes opens the menu; the lane's x removes it
    send({r.x + 60.0f, laneTop + 56.0f + 15.0f}, true, true);
    send({r.x + 60.0f, laneTop + 56.0f + 15.0f}, false, false);
    c.check(menuTrack == 0, "Automation lane: the add strip opens the menu of what the track can automate");
    send({r.x + 248.0f, laneTop + 28.0f}, true, true);
    send({r.x + 248.0f, laneTop + 28.0f}, false, false);
    c.check(store.project().tracks[0].automation.empty(), "Automation lane: its x removes the lane");
}

// Markers in the arrangement, by input handed to the view as above. The markers lane is under the
// ruler (30) and a gap (4), 24 tall. A double-click in empty space adds one (and asks for its name), a
// click on one puts the playhead there, a drag moves it, a right-click opens its menu; with the global
// lanes hidden, a click on its flag in the ruler puts the playhead there.
void markerEditChecks(Checker &c)
{
    engine::Window window(1200, 520, "VYBN markers test", false);
    window.setPosition(-32000, -32000);
    engine::Renderer renderer(window, false, true);
    const ui::Theme theme = daw::studioTheme(true);
    daw::ProjectStore store(daw::demoProject());
    double sought = -1.0;
    daw::ObjectId renamed = daw::kNoId, menu = daw::kNoId;
    daw::TransportHooks hooks = quietTransport();
    hooks.seek = [&sought](double beat) { sought = beat; };
    hooks.renameMarker = [&renamed](daw::ObjectId id) { renamed = id; };
    hooks.markerMenu = [&menu](daw::ObjectId id, double, ui::Vec2) { menu = id; };
    ui::Context context(window, renderer, theme);
    auto &view = context.root().add<daw::ArrangementView>(store, hooks);
    stretch(view);
    auto frame = [&] {
        context.update();
        context.buildFrame();
    };
    for (int i = 0; i < 4; ++i)
        frame();
    const ui::Rect r = view.rect();
    const float wellX = r.x + 278.0f, laneY = r.y + 34.0f + 12.0f;
    auto beatX = [wellX](double beat) { return wellX + static_cast<float>(beat) * 22.0f; };
    double now = 10.0;
    auto send = [&](ui::Vec2 at, bool down, bool pressed, bool secondary = false) {
        ui::Input in;
        in.mouse = at;
        in.down = down;
        in.pressed = pressed;
        in.secondaryPressed = secondary;
        in.time = now;
        view.handleInput(in);
        view.arrange(view.rect(), theme);
        now += 0.5;
    };

    // A double-click at beat 8.2: a marker at 8, its name asked for
    const double first = now;
    send({beatX(8.2), laneY}, true, true);
    send({beatX(8.2), laneY}, false, false);
    now = first + 0.2;
    send({beatX(8.2), laneY}, true, true);
    send({beatX(8.2), laneY}, false, false);
    const auto &markers = store.project().markers;
    c.check(markers.size() == 1 && markers[0].beat == 8.0 && markers[0].name == "Marker 1" && markers[0].id != daw::kNoId &&
                renamed == markers[0].id,
            "Markers lane: a double-click adds a marker and asks for its name", std::to_string(markers.size()));
    frame(); // the flag is drawn (its place is where it was drawn)

    // A click on it puts the playhead there; a drag moves it to beat 12
    send({beatX(8.0) + 10.0f, laneY}, true, true);
    const bool located = sought == 8.0;
    send({beatX(12.0) + 10.0f, laneY}, true, false);
    send({beatX(12.0) + 10.0f, laneY}, false, false);
    c.check(located && markers.size() == 1 && markers[0].beat == 12.0, "Markers lane: a click locates, a drag moves",
            markers.empty() ? "" : std::to_string(markers[0].beat) + (located ? " located" : " not located, at " + std::to_string(sought)));
    frame();

    // Its menu on a right-click; the lane's (no marker) on empty space
    send({beatX(12.0) + 10.0f, laneY}, false, false, true);
    const bool own = menu == markers[0].id;
    send({beatX(30.0), laneY}, false, false, true);
    c.check(own && menu == daw::kNoId, "Markers lane: a right-click opens a marker's menu, or the lane's");

    // With the lanes hidden, its flag in the ruler locates
    view.showTiming(false);
    for (int i = 0; i < 3; ++i)
        frame();
    sought = -1.0;
    send({beatX(12.0) + 8.0f, r.y + 20.0f}, true, true);
    send({beatX(12.0) + 8.0f, r.y + 20.0f}, false, false);
    c.check(sought == 12.0, "Markers: a flag in the ruler locates with the lanes hidden", std::to_string(sought));
}

// Editing the tempo map in the arrangement's tempo and signature lanes, by input handed to the view as
// above. Under the ruler (30) and a gap (4): the markers lane (24), a gap (4), the signature lane (26), a
// gap (4), the tempo lane (64, its curve 8 in from top and bottom on the lanes' BPM scale).
void tempoEditChecks(Checker &c)
{
    engine::Window window(1200, 520, "VYBN tempo test", false);
    window.setPosition(-32000, -32000);
    engine::Renderer renderer(window, false, true);
    const ui::Theme theme = daw::studioTheme(true);
    daw::ProjectStore store(daw::demoProject());
    int pointMenu = -1, signatureMenu = -1, edited = -1;
    daw::TransportHooks hooks = quietTransport();
    hooks.tempoMenu = [&pointMenu](int point, ui::Vec2) { pointMenu = point; };
    hooks.signatureMenu = [&signatureMenu](int signature, ui::Vec2) { signatureMenu = signature; };
    hooks.editSignature = [&edited](int signature) { edited = signature; };
    ui::Context context(window, renderer, theme);
    auto &view = context.root().add<daw::ArrangementView>(store, hooks);
    stretch(view);
    for (int i = 0; i < 4; ++i) {
        context.update();
        context.buildFrame();
    }

    const ui::Rect r = view.rect();
    const float wellX = r.x + 278.0f, signatureY = r.y + 62.0f + 13.0f, tempoTop = r.y + 92.0f;
    auto beatX = [wellX](double beat) { return wellX + static_cast<float>(beat) * 22.0f; };
    auto bpmY = [&view, tempoTop](double bpm) {
        double low = 0.0, high = 0.0;
        view.timingLanes().tempoRange(low, high);
        return tempoTop + 56.0f - static_cast<float>((bpm - low) / (high - low)) * 48.0f;
    };
    double now = 10.0;
    auto send = [&](ui::Vec2 at, bool down, bool pressed, bool secondary = false) {
        ui::Input in;
        in.mouse = at;
        in.down = down;
        in.pressed = pressed;
        in.secondaryPressed = secondary;
        in.time = now;
        view.handleInput(in);
        view.arrange(view.rect(), theme);
        now += 0.5;
    };
    auto points = [&store] { return store.project().timing.points(); };
    auto signatures = [&store] { return store.project().timing.signatures(); };

    // A click at beat 8.3 where 130 BPM is: a point at beat 8 (snapped), at 130
    send({beatX(8.3), bpmY(130.0)}, true, true);
    send({beatX(8.3), bpmY(130.0)}, false, false);
    std::vector<audio::TempoPoint> p = points();
    c.check(p.size() == 2 && p[1].beat == 8.0 && std::fabs(p[1].bpm - 130.0) <= 1.0, "Tempo lane: a click adds a point, snapped to the beat",
            p.size() < 2 ? "no point" : std::to_string(p[1].beat) + " @ " + std::to_string(p[1].bpm));

    // Dragging it along to beat 12, keeping its tempo
    send({beatX(8.0), bpmY(p[1].bpm)}, true, true);
    send({beatX(12.2), bpmY(p[1].bpm)}, true, false);
    send({beatX(12.2), bpmY(p[1].bpm)}, false, false);
    const double kept = p.size() == 2 ? p[1].bpm : 0.0;
    p = points();
    c.check(p.size() == 2 && p[1].beat == 12.0 && std::fabs(p[1].bpm - kept) <= 1.0, "Tempo lane: dragging a point moves it",
            p.size() < 2 ? "no point" : std::to_string(p[1].beat) + " @ " + std::to_string(p[1].bpm));

    // The first point moves only in tempo
    send({beatX(0.0), bpmY(120.0)}, true, true);
    send({beatX(4.0), bpmY(110.0)}, true, false);
    send({beatX(4.0), bpmY(110.0)}, false, false);
    p = points();
    c.check(p.size() == 2 && p[0].beat == 0.0 && p[0].bpm < 120.0, "Tempo lane: the first point stays at the start",
            std::to_string(p[0].beat) + " @ " + std::to_string(p[0].bpm));

    // A right-click on a point opens its menu
    send({beatX(12.0), bpmY(p[1].bpm)}, false, false, true);
    c.check(pointMenu == 1, "Tempo lane: a right-click opens a point's menu", std::to_string(pointMenu));

    // A double-click in bar 3 adds a signature change there and opens its editor
    const double first = now;
    send({beatX(9.0), signatureY}, true, true);
    send({beatX(9.0), signatureY}, false, false);
    now = first + 0.2;
    send({beatX(9.0), signatureY}, true, true);
    send({beatX(9.0), signatureY}, false, false);
    std::vector<audio::Signature> s = signatures();
    c.check(s.size() == 2 && s[1].bar == 2 && s[1].numerator == 4 && edited == 1, "Signature lane: a double-click adds a change at the bar",
            std::to_string(s.size()) + " signatures, editor " + std::to_string(edited));

    // Dragging its marker to bar 5; the first marker doesn't move
    send({beatX(8.0) + 10.0f, signatureY}, true, true);
    send({beatX(16.3) + 10.0f, signatureY}, true, false);
    send({beatX(16.3) + 10.0f, signatureY}, false, false);
    send({beatX(0.0) + 10.0f, signatureY}, true, true);
    send({beatX(4.0) + 10.0f, signatureY}, true, false);
    send({beatX(4.0) + 10.0f, signatureY}, false, false);
    s = signatures();
    c.check(s.size() == 2 && s[0].bar == 0 && s[1].bar == 4, "Signature lane: a marker drags along the bars, the first stays",
            s.size() < 2 ? "" : "bar " + std::to_string(s[1].bar));

    // A right-click on a marker opens its menu
    send({beatX(16.0) + 10.0f, signatureY}, false, false, true);
    c.check(signatureMenu == 1, "Signature lane: a right-click opens a change's menu", std::to_string(signatureMenu));

    // A click selects a point (for Delete); a press elsewhere lets go of it
    p = points();
    send({beatX(12.0), bpmY(p[1].bpm)}, true, true);
    send({beatX(12.0), bpmY(p[1].bpm)}, false, false);
    const int selected = view.timingLanes().selectedPoint();
    send({r.x + 600.0f, r.y + 300.0f}, true, true);
    send({r.x + 600.0f, r.y + 300.0f}, false, false);
    c.check(selected == 1 && view.timingLanes().selectedPoint() == -1, "Tempo lane: a click selects a point, a press elsewhere lets go",
            std::to_string(selected));

    // Right-clicking empty lane space opens the lane's menu at the beat
    double menuBeat = -1.0;
    bool menuTempo = false;
    hooks.timingLaneMenu = [&](ui::Vec2, double beat, bool tempo) {
        menuBeat = beat;
        menuTempo = tempo;
    };
    {
        daw::ArrangementView other(store, hooks); // laid out where the first one is, outside the tree
        other.arrange(view.rect(), theme);
        ui::Input in;
        in.mouse = {beatX(20.2), bpmY(100.0)};
        in.secondaryPressed = true;
        in.time = now;
        other.handleInput(in);
        c.check(menuBeat == 20.0 && menuTempo, "Tempo lane: a right-click on empty space opens the lane's menu",
                std::to_string(menuBeat));
    }

    // The corner chip hides the lanes and shows them again
    send({r.x + 60.0f, r.y + 15.0f}, true, true);
    const bool hidden = !view.timingShown();
    send({r.x + 60.0f, r.y + 15.0f}, false, false);
    send({r.x + 60.0f, r.y + 15.0f}, true, true);
    c.check(hidden && view.timingShown(), "Arrangement: the corner chip hides and shows the tempo lanes");
}

// The master's row, by input handed to the view as above: two tracks, then the master's header (name and
// A toggle, 56 pixels) and, shown, its volume lane. A click in the lane adds a point to the project's
// master lane; the add strip opens the master's menu (not a track's); the lane's x removes it; and the
// row is never a track: trackAt reports it as the space below the tracks, and pressing and dragging its
// header, or pressing in its lane area, selects, moves and adds nothing.
void masterEditChecks(Checker &c)
{
    engine::Window window(1200, 520, "VYBN master automation test", false);
    window.setPosition(-32000, -32000);
    engine::Renderer renderer(window, false, true);
    const ui::Theme theme = daw::studioTheme(true);
    daw::Project project = daw::demoProject();
    project.tracks.resize(2);
    project.masterAutomation.push_back({audio::kVolumeParam, {}});
    daw::ProjectStore store(project);
    bool masterMenu = false, trackMenu = false;
    daw::TransportHooks hooks = quietTransport();
    hooks.masterAutomationMenu = [&masterMenu](ui::Vec2) { masterMenu = true; };
    hooks.automationMenu = [&trackMenu](int, ui::Vec2) { trackMenu = true; };
    ui::Context context(window, renderer, theme);
    auto &view = context.root().add<daw::ArrangementView>(store, hooks);
    view.showTiming(false); // the tempo lanes have their own scene
    stretch(view);
    view.showMasterAutomation(true);
    for (int i = 0; i < 4; ++i) {
        context.update();
        context.buildFrame();
    }
    const ui::Rect r = view.rect();
    const float wellX = r.x + 278.0f, masterTop = r.y + 30.0f + 2 * 76.0f, laneTop = masterTop + 56.0f;
    auto beatX = [wellX](double beat) { return wellX + static_cast<float>(beat) * 22.0f; };
    auto laneY = [laneTop](float position) { return laneTop + 7.0f + (1.0f - position) * (56.0f - 14.0f); };
    double now = 10.0;
    auto send = [&](ui::Vec2 at, bool down, bool pressed, bool secondary = false) {
        ui::Input in;
        in.mouse = at;
        in.down = down;
        in.pressed = pressed;
        in.secondaryPressed = secondary;
        in.time = now;
        view.handleInput(in);
        view.arrange(view.rect(), theme);
        now += 0.5;
    };
    auto points = [&store]() -> std::vector<audio::AutomationPoint> {
        const audio::AutomationLane *lane = audio::findAutomation(store.project().masterAutomation, audio::kVolumeParam);
        return lane ? lane->points : std::vector<audio::AutomationPoint>{};
    };

    send({beatX(4.3), laneY(0.5f)}, true, true);
    send({beatX(4.3), laneY(0.5f)}, false, false);
    std::vector<audio::AutomationPoint> p = points();
    c.check(p.size() == 1 && p[0].beat == 4.0 && std::fabs(p[0].value - 0.5f) < 0.03f,
            "Master row: a click in the master's volume lane adds a point to the project's master lane",
            p.empty() ? "no point" : std::to_string(p[0].beat) + " @ " + std::to_string(p[0].value));
    send({beatX(4.0), laneY(0.5f)}, true, true);
    send({beatX(8.2), laneY(0.9f)}, true, false);
    send({beatX(8.2), laneY(0.9f)}, false, false);
    p = points();
    c.check(p.size() == 1 && p[0].beat == 8.0 && std::fabs(p[0].value - 0.9f) < 0.03f, "Master row: dragging a point moves it",
            p.empty() ? "no point" : std::to_string(p[0].beat) + " @ " + std::to_string(p[0].value));
    send({beatX(8.0), laneY(0.9f)}, false, false, true);
    c.check(points().empty(), "Master row: a right-click removes a point");

    // The master's row is not a track
    const ui::Vec2 inHeader{r.x + 60.0f, masterTop + 28.0f};
    const ui::Vec2 inLane{beatX(30.0), masterTop + 28.0f};
    const int below = view.trackAt(inHeader), laneBelow = view.trackAt(inLane);
    const std::vector<daw::ObjectId> order{store.project().tracks[0].id, store.project().tracks[1].id};
    const int selected = store.project().selectedTrack;
    const size_t clips0 = store.project().tracks[0].clips.size(), clips1 = store.project().tracks[1].clips.size();
    send(inHeader, true, true);
    send({inHeader.x, r.y + 60.0f}, true, false); // dragged up over the tracks
    send({inHeader.x, r.y + 60.0f}, false, false);
    send(inLane, true, true);
    send(inLane, false, false);
    send(inLane, true, true); // a double-click there adds no clip either
    send(inLane, false, false);
    send(inLane, false, false, true);
    c.check(below == 2 && laneBelow == 2 && store.project().tracks.size() == 2 && store.project().tracks[0].id == order[0] &&
                store.project().tracks[1].id == order[1] && store.project().selectedTrack == selected &&
                store.project().tracks[0].clips.size() == clips0 && store.project().tracks[1].clips.size() == clips1,
            "Master row: it is not a track (trackAt says the space below, nothing selected, moved or added)",
            std::to_string(below) + ", " + std::to_string(laneBelow));

    // The add strip opens the master's menu; the lane's x removes the lane
    send({r.x + 60.0f, laneTop + 56.0f + 15.0f}, true, true);
    send({r.x + 60.0f, laneTop + 56.0f + 15.0f}, false, false);
    c.check(masterMenu && !trackMenu, "Master row: the add strip opens the master's menu, not a track's");
    send({r.x + 248.0f, laneTop + 28.0f}, true, true);
    send({r.x + 248.0f, laneTop + 28.0f}, false, false);
    c.check(store.project().masterAutomation.empty(), "Master row: its x removes the master's lane");
}

// Folding a group in the arrangement, by the view's layout and lookups: tracks 0 and 4 outside, a group
// at 1 holding the tracks at 2 and 3. Folding takes the two rows (76 pixels each) out of the layout,
// so the row below moves up by 152 and trackAt finds the tracks that are left; unfolding puts them
// back. Only a group folds, and a hidden track's open lanes take no room either.
void foldChecks(Checker &c)
{
    engine::Window window(1200, 900, "VYBN fold test", false);
    window.setPosition(-32000, -32000);
    engine::Renderer renderer(window, false, true);
    const ui::Theme theme = daw::studioTheme(true);
    daw::Project project = daw::demoProject();
    project.tracks.resize(5);
    project.addTrack(daw::TrackKind::Group);
    std::rotate(project.tracks.begin() + 1, project.tracks.begin() + 5, project.tracks.begin() + 6);
    daw::ObjectId nextId = 1;
    project.normalizeIds(nextId);
    const daw::ObjectId group = project.tracks[1].id;
    project.tracks[2].output = group;
    project.tracks[3].output = group;
    daw::ProjectStore store(project);
    ui::Context context(window, renderer, theme);
    auto &view = context.root().add<daw::ArrangementView>(store, quietTransport());
    view.showTiming(false); // the tempo lanes have their own scene
    stretch(view);
    auto settle = [&] {
        for (int i = 0; i < 3; ++i) {
            context.update();
            context.buildFrame();
        }
    };
    settle();
    const ui::Rect r = view.rect();
    // A point in the header column in the middle of the unfolded layout's `rows`th row
    auto at = [&r](float rows) { return ui::Vec2{r.x + 60.0f, r.y + 30.0f + 76.0f * rows + 30.0f}; };

    const int before = view.trackAt(at(4.0f)), middle = view.trackAt(at(2.0f));
    c.check(before == 4 && middle == 2 && !view.folded(1) && !view.hidden(2), "Fold: unfolded, every row is found (trackAt)",
            std::to_string(before) + ", " + std::to_string(middle));

    view.fold(0, true); // not a group
    view.fold(2, true);
    c.check(!view.folded(0) && !view.folded(2) && !view.folded(99), "Fold: only a group folds");

    view.fold(1, true);
    settle();
    const int moved = view.trackAt(at(2.0f)), group1 = view.trackAt(at(1.0f));
    c.check(view.folded(1) && view.hidden(2) && view.hidden(3) && !view.hidden(1) && !view.hidden(4),
            "Fold: folding a group hides the tracks that go into it");
    c.check(moved == 4 && group1 == 1, "Fold: the row below moves up by the hidden rows' heights, and the group stays",
            std::to_string(moved) + ", " + std::to_string(group1));
    bool found = false;
    for (float y = 0.0f; y < 76.0f * 6.0f; y += 7.0f) {
        const int t = view.trackAt({r.x + 60.0f, r.y + 30.0f + y});
        found = found || t == 2 || t == 3;
    }
    c.check(!found, "Fold: no point on the timeline finds a hidden track");

    // A press where the hidden header was reaches what is there now, never the hidden track
    {
        ui::Input in;
        in.mouse = at(2.0f);
        in.pressed = in.down = true;
        in.time = 5.0;
        view.handleInput(in);
        c.check(store.project().selectedTrack != 2 && store.project().selectedTrack != 3,
                "Fold: a press in the header column never selects a hidden track", std::to_string(store.project().selectedTrack));
    }

    view.fold(1, false);
    settle();
    c.check(!view.folded(1) && !view.hidden(2) && view.trackAt(at(2.0f)) == 2 && view.trackAt(at(4.0f)) == 4,
            "Fold: unfolding brings the rows back where they were");

    view.showAutomation(2, true);
    view.fold(1, true);
    settle();
    c.check(view.trackAt(at(2.0f)) == 4, "Fold: a hidden track's open lanes take no room either",
            std::to_string(view.trackAt(at(2.0f))));
}

// A project with a group nested in a group: A, Outer (holding Inner and C), Inner (holding B), B, C, D
daw::Project nestedGroupsProject()
{
    daw::Project p = daw::demoProject();
    p.tracks.resize(4);
    p.addTrack(daw::TrackKind::Group);
    p.addTrack(daw::TrackKind::Group);
    p.tracks[4].name = "Outer";
    p.tracks[5].name = "Inner";
    std::vector<daw::Track> order;
    for (size_t i : {0u, 4u, 5u, 1u, 2u, 3u})
        order.push_back(p.tracks[i]);
    p.tracks = order;
    daw::ObjectId next = 1;
    p.normalizeIds(next);
    const daw::ObjectId outer = p.tracks[1].id, inner = p.tracks[2].id;
    p.tracks[2].output = outer; // Inner into Outer
    p.tracks[3].output = inner; // B into Inner
    p.tracks[4].output = outer; // C into Outer
    return p;
}

// The rest of the folding: the shared state's version, the chevron, indentation, the selection
// unfolding, the folded counts, and the mixer following the same state
void foldUiChecks(Checker &c)
{
    {
        daw::ProjectStore base(nestedGroupsProject());
        daw::FoldState state(base);
        const daw::ObjectId outer = base.project().tracks[1].id, inner = base.project().tracks[2].id,
                            plain = base.project().tracks[0].id;
        const uint64_t v0 = state.version();
        const bool a = state.set(outer, true);
        const uint64_t v1 = state.version();
        const bool b = state.set(outer, true); // no change
        const uint64_t v2 = state.version();
        state.set(outer, false);
        state.set(outer, false); // no change
        const uint64_t v3 = state.version();
        const bool notGroup = state.set(plain, true); // only a group folds
        state.set(inner, true);
        state.set(outer, true);
        const uint64_t v4 = state.version();
        // Another way to fold (a project replaced by one with a different set folded) moves it too
        daw::Project other = nestedGroupsProject();
        other.tracks[2].folded = true;
        base.replace(other);
        const uint64_t v5 = state.version();
        state.clear();
        const uint64_t v6 = state.version();
        state.clear(); // none left
        c.check(a && !b && !notGroup && v1 > v0 && v2 == v1 && v3 > v2 && v4 > v3 && v5 > v4 && v6 > v5 &&
                    state.version() == v6 && state.groups().empty(),
                "Fold state: its version moves on every change of the folded set and only then");
    }

    engine::Window window(1200, 900, "VYBN fold ui test", false);
    window.setPosition(-32000, -32000);
    engine::Renderer renderer(window, false, true);
    const ui::Theme theme = daw::studioTheme(true);
    daw::ProjectStore store(nestedGroupsProject());
    daw::FoldState folds(store);
    ui::Context context(window, renderer, theme);
    auto &view = context.root().add<daw::ArrangementView>(store, quietTransport(), &folds);
    view.showTiming(false); // the tempo lanes have their own scene
    stretch(view);
    auto settle = [&] {
        for (int i = 0; i < 3; ++i) {
            context.update();
            context.buildFrame();
        }
    };
    settle();

    // Nested tracks hang to the right of the ones above them
    const daw::TrackHeader *top = view.header(0), *outer = view.header(1), *inner = view.header(2), *deep = view.header(3);
    c.check(top && outer && inner && deep && view.depth(3) == 2 && view.depth(4) == 1 && view.depth(5) == 0 &&
                outer->rect().x == top->rect().x && inner->rect().x > outer->rect().x && deep->rect().x > inner->rect().x &&
                view.header(4)->rect().x > top->rect().x && view.header(4)->rect().x < deep->rect().x,
            "Fold: a nested track's header is indented by its depth",
            top && deep ? std::to_string(top->rect().x) + " .. " + std::to_string(deep->rect().x) : "no header");

    // The chevron: a click folds the group, another unfolds it
    auto click = [&](ui::Vec2 at) {
        ui::Input in;
        in.mouse = at;
        in.down = in.pressed = true;
        in.time = 3.0;
        view.handleInput(in);
        ui::Input up;
        up.mouse = at;
        up.released = true;
        up.time = 3.1;
        view.handleInput(up);
        settle();
    };
    const ui::Rect chev = view.header(1)->chevronRect();
    const bool hasChevron = chev.w > 0.0f && view.header(0)->chevronRect().w == 0.0f;
    click({chev.x + chev.w * 0.5f, chev.y + chev.h * 0.5f});
    c.check(hasChevron && view.folded(1) && view.hidden(2) && view.hidden(3) && view.hidden(4) && !view.hidden(5) && folds.folded(store.project().tracks[1].id),
            "Fold: clicking a group's chevron folds it (a track has none), through the shared state");
    c.check(view.foldedCount(1) == 3 && view.foldedCount(2) == 0 && view.foldedCount(0) == 0,
            "Fold: a folded group counts the tracks inside it, nested ones included", std::to_string(view.foldedCount(1)));
    const ui::Rect again = view.header(1)->chevronRect();
    click({again.x + again.w * 0.5f, again.y + again.h * 0.5f});
    c.check(!view.folded(1) && !view.hidden(3), "Fold: clicking it again unfolds");

    // Selecting a hidden track unfolds the groups hiding it, once per selection change
    view.fold(2, true);
    view.fold(1, true);
    settle();
    const bool hiddenBefore = view.hidden(3);
    store.select(3);
    settle();
    c.check(hiddenBefore && !view.hidden(3) && !view.folded(1) && !view.folded(2),
            "Fold: selecting a hidden track unfolds the groups that hid it");
    view.fold(1, true);
    settle();
    c.check(view.folded(1) && view.hidden(3), "Fold: the selection unfolds once, not every frame");
    store.select(5);
    settle();
    c.check(view.folded(1), "Fold: selecting a track that isn't hidden leaves groups folded");
    view.unfoldAll();
    view.foldAll();
    c.check(view.folded(1) && view.folded(2) && !view.folded(0), "Fold: Fold all folds every group");
    view.unfoldAll();
    c.check(!view.folded(1) && !view.folded(2) && folds.groups().empty(), "Fold: Unfold all unfolds them");
}

// Folding as part of the project, and dragging headers while groups are folded: a project opened with a
// folded group hides its tracks (the arrangement's rows and the mixer's strips), dragging a folded group's
// header moves the group with the tracks hidden in it, and a track dragged to a spot among hidden tracks
// lands before the group or after them
void foldDragChecks(Checker &c)
{
    engine::Window window(1200, 900, "VYBN fold drag test", false);
    window.setPosition(-32000, -32000);
    engine::Renderer renderer(window, false, true);
    const ui::Theme theme = daw::studioTheme(true);

    // Opened folded: the layout starts with the group's tracks hidden
    {
        daw::Project project = nestedGroupsProject();
        project.tracks[2].folded = true; // Inner: hides B
        daw::ProjectStore store(project);
        ui::Context context(window, renderer, theme);
        auto &view = context.root().add<daw::ArrangementView>(store, quietTransport());
        view.showTiming(false); // the tempo lanes have their own scene
        stretch(view);
        for (int i = 0; i < 3; ++i) {
            context.update();
            context.buildFrame();
        }
        const ui::Rect r = view.rect();
        // Rows: A, Outer, Inner (folded), C, D (B has no height)
        const int at3 = view.trackAt({r.x + 60.0f, r.y + 30.0f + 76.0f * 3 + 30.0f});
        c.check(view.folded(2) && !view.folded(1) && view.hidden(3) && view.header(3) == nullptr && at3 == 4,
                "Fold: a project opened with a folded group has its tracks hidden", std::to_string(at3));
    }

    // Dragging a folded group's header moves it with its hidden tracks
    {
        daw::Project project = nestedGroupsProject();
        project.tracks[1].folded = true; // Outer: hides Inner, B and C
        daw::ProjectStore store(project);
        std::vector<daw::ObjectId> ids;
        for (const daw::Track &t : store.project().tracks)
            ids.push_back(t.id);
        ui::Context context(window, renderer, theme);
        auto &view = context.root().add<daw::ArrangementView>(store, quietTransport());
        view.showTiming(false); // the tempo lanes have their own scene
        stretch(view);
        auto settle = [&] {
            for (int i = 0; i < 3; ++i) {
                context.update();
                context.buildFrame();
            }
        };
        settle();
        const ui::Rect r = view.rect();
        double now = 20.0;
        auto send = [&](ui::Vec2 at, bool down, bool pressed) {
            ui::Input in;
            in.mouse = at;
            in.down = down;
            in.pressed = pressed;
            in.released = !down;
            in.time = now;
            view.handleInput(in);
            view.arrange(view.rect(), theme);
            now += 0.5;
        };
        // Rows now: A (0), Outer (1, folded), D (5); drag Outer from its row to below D
        const float x = r.x + 100.0f;
        const float outerY = r.y + 30.0f + 76.0f + 38.0f, belowD = r.y + 30.0f + 200.0f;
        send({x, outerY}, true, true);
        send({x, outerY + 10.0f}, true, false);
        send({x, belowD}, true, false);
        send({x, belowD}, false, false);
        settle();
        const std::vector<daw::Track> &t = store.project().tracks;
        const bool order = t.size() == 6 && t[0].id == ids[0] && t[1].id == ids[5] && t[2].id == ids[1] && t[3].id == ids[2] &&
                           t[4].id == ids[3] && t[5].id == ids[4];
        c.check(order && t[2].folded && view.hidden(3) && view.hidden(5) && !view.hidden(1),
                "Fold: dragging a folded group's header moves it with the tracks hidden in it, in order");

        // Dragging the first track (A) down to between the folded group and where its hidden tracks were
        // lands it before the group (the nearer end), not among them
        const float dY = r.y + 30.0f + 38.0f; // A's row
        send({x, dY}, true, true);
        send({x, dY + 10.0f}, true, false);
        const float underOuter = r.y + 30.0f + 196.0f; // its middle falls past Outer, before where the hidden tracks were
        send({x, underOuter}, true, false);
        send({x, underOuter}, false, false);
        settle();
        const std::vector<daw::Track> &u = store.project().tracks;
        c.check(u.size() == 6 && u[0].id == ids[5] && u[1].id == ids[0] && u[2].id == ids[1] && u[3].id == ids[2],
                "Fold: a track dropped where a folded group's hidden tracks are never lands among them");
    }
}

// The mixer following the shared fold state: a folded group's tracks have no strips, and its own has a chevron
void foldMixerChecks(Checker &c)
{
    env::Environment environment;
    audio::AudioSystem engine(environment, audio::Deviceless{});
    engine::Window window(1200, 400, "VYBN fold mixer test", false);
    window.setPosition(-32000, -32000);
    engine::Renderer renderer(window, false, true);
    const ui::Theme theme = daw::studioTheme(true);
    daw::ProjectStore store(nestedGroupsProject());
    daw::FoldState folds(store);
    const std::vector<float> levels(6, 0.0f);
    const daw::StereoLevels stereo{};
    ui::Context context(window, renderer, theme);
    auto &mixer = context.root().add<daw::MixerView>(context, store, engine, stereo, levels, [](daw::TrackKind) {}, nullptr, &folds);
    stretch(mixer);
    auto settle = [&] {
        for (int i = 0; i < 4; ++i) {
            context.update();
            context.buildFrame();
        }
    };
    settle();
    const size_t all = mixer.stripCount();
    folds.set(store.project().tracks[2].id, true); // Inner: hides B
    settle();
    const size_t inner = mixer.stripCount();
    folds.set(store.project().tracks[1].id, true); // Outer: hides Inner, B and C
    settle();
    const size_t outer = mixer.stripCount();
    folds.clear();
    settle();
    c.check(all == 6 && inner == 5 && outer == 3 && mixer.stripCount() == 6,
            "Fold: the mixer shows fewer strips while groups are folded, and all again after",
            std::to_string(all) + ", " + std::to_string(inner) + ", " + std::to_string(outer) + ", " + std::to_string(mixer.stripCount()));

    // With no arrangement at all, a selection that lands on a hidden track (B, inside Inner inside Outer)
    // still unfolds the groups hiding it, and its strip comes back
    folds.set(store.project().tracks[2].id, true);
    folds.set(store.project().tracks[1].id, true);
    settle();
    const size_t folded = mixer.stripCount();
    store.select(3);
    settle();
    c.check(folded == 3 && !folds.folded(store.project().tracks[1].id) && !folds.folded(store.project().tracks[2].id) &&
                mixer.stripCount() == 6,
            "Fold: the mixer alone unfolds the groups hiding a selected track",
            std::to_string(folded) + " strips, then " + std::to_string(mixer.stripCount()));
}

// The arrangement with folding: a group (Outer) open round a folded group (Inner, its Bass clip shown as a
// block in its row, "1 track folded" in its caption) and a track, one of Outer's tracks with its volume lane
// open (so its lane header is indented too). Headers are indented by depth with a bar for each level, and
// the chevrons point down (open) and right (folded). With `after`, one more frame is run after the picture.
Picture renderFolding(ui::FrameStats *after)
{
    daw::Project project = nestedGroupsProject();
    project.tracks[4].automation.push_back({audio::kVolumeParam, {{0.0, 0.5f}, {10.0, 0.8f}, {20.0, 0.3f}}});
    daw::ProjectStore store(project); // outlives the context
    daw::FoldState folds(store);      //   (and so does this)
    return renderPicture(1200, 740, daw::studioTheme(true), [&store, &folds](ui::Context &context) {
        auto &view = context.root().add<daw::ArrangementView>(store, quietTransport(), &folds);
        view.showTiming(false); // the tempo lanes have their own scene
        stretch(view);
        view.fold(2, true);
        view.showAutomation(4, true);
    }, 0, nullptr, after);
}

Picture sceneFolding()
{
    return renderFolding(nullptr);
}

// The first component of type T under `parent` (a scroll view's content included), or null
template <typename T>
T *findIn(ui::Component &parent)
{
    if (auto *hit = dynamic_cast<T *>(&parent))
        return hit;
    if (auto *scroll = dynamic_cast<ui::ScrollView *>(&parent))
        return findIn<T>(scroll->content());
    if (auto *container = dynamic_cast<ui::Container *>(&parent)) {
        for (size_t i = 0; i < container->childCount(); ++i)
            if (T *hit = findIn<T>(container->child(i)))
                return hit;
    }
    return nullptr;
}

// Every label's text under `parent`, joined (to look for a caption)
void labelsIn(ui::Component &parent, std::string &out)
{
    if (auto *label = dynamic_cast<ui::Label *>(&parent))
        out += label->text() + "\n";
    if (auto *scroll = dynamic_cast<ui::ScrollView *>(&parent))
        labelsIn(scroll->content(), out);
    else if (auto *container = dynamic_cast<ui::Container *>(&parent))
        for (size_t i = 0; i < container->childCount(); ++i)
            labelsIn(container->child(i), out);
}

// How many send bars the strips under `parent` hold (each is a child of a strip's ListView of sends)
size_t sendBarsIn(ui::Component &parent)
{
    size_t n = 0;
    if (auto *list = dynamic_cast<ui::ListView *>(&parent))
        return list->childCount();
    if (auto *scroll = dynamic_cast<ui::ScrollView *>(&parent))
        return sendBarsIn(scroll->content());
    if (auto *container = dynamic_cast<ui::Container *>(&parent)) {
        for (size_t i = 0; i < container->childCount(); ++i)
            n += sendBarsIn(container->child(i));
    }
    return n;
}

// Flexible routing in the mixer: a track sending to a group gets a send bar for it (only while the send exists),
// and its output label counts the groups it also goes to
void flexibleMixerChecks(Checker &c)
{
    env::Environment environment;
    audio::AudioSystem engine(environment, audio::Deviceless{});
    engine::Window window(1200, 400, "VYBN flexible mixer test", false);
    window.setPosition(-32000, -32000);
    engine::Renderer renderer(window, false, true);
    const ui::Theme theme = daw::studioTheme(true);
    daw::Project p;
    p.addTrack(daw::TrackKind::Audio);
    p.addTrack(daw::TrackKind::Return);
    p.addTrack(daw::TrackKind::Group);
    p.addTrack(daw::TrackKind::Group);
    daw::ObjectId next = 1;
    p.normalizeIds(next);
    const daw::ObjectId g1 = p.tracks[2].id, g2 = p.tracks[3].id;
    daw::ProjectStore store(p);
    const std::vector<float> levels(4, 0.0f);
    const daw::StereoLevels stereo{};
    ui::Context context(window, renderer, theme);
    auto &mixer = context.root().add<daw::MixerView>(context, store, engine, stereo, levels, [](daw::TrackKind) {});
    stretch(mixer);
    auto settle = [&] {
        for (int i = 0; i < 4; ++i) {
            context.update();
            context.buildFrame();
        }
    };
    auto text = [&] {
        std::string out;
        labelsIn(mixer, out);
        return out;
    };
    settle();
    // The 3 other buses give a track a bar each for the return only: 1 (the return); the groups' strips have 1 too
    const size_t before = sendBarsIn(mixer);
    store.editMix(0).output = g1;
    daw::setAlsoTo(store, 0, g2, true);
    settle();
    const size_t after = sendBarsIn(mixer);
    const std::string shown = text();
    c.check(after == before + 1 && shown.find("To Group 1 + 1") != std::string::npos,
            "Flexible routing: a send into a group gets a bar in the strip, and the output label counts it",
            std::to_string(before) + " -> " + std::to_string(after));
    daw::setAlsoTo(store, 0, g2, false);
    settle();
    c.check(sendBarsIn(mixer) == before && text().find("To Group 1\n") != std::string::npos,
            "Flexible routing: unticking takes the bar and the label's count away");
}

// A strip with many send bars keeps its toggles inside it: 12 returns are 11 bars (or 10 for a return), which scroll
void tallStripChecks(Checker &c)
{
    env::Environment environment;
    audio::AudioSystem engine(environment, audio::Deviceless{});
    engine::Window window(1200, 400, "VYBN tall strip test", false);
    window.setPosition(-32000, -32000);
    engine::Renderer renderer(window, false, true);
    const ui::Theme theme = daw::studioTheme(true);
    daw::Project p;
    p.addTrack(daw::TrackKind::Audio);
    for (int i = 0; i < 12; ++i)
        p.addTrack(daw::TrackKind::Return);
    daw::ObjectId next = 1;
    p.normalizeIds(next);
    daw::ProjectStore store(p);
    const std::vector<float> levels(13, 0.0f);
    const daw::StereoLevels stereo{};
    ui::Context context(window, renderer, theme);
    auto &mixer = context.root().add<daw::MixerView>(context, store, engine, stereo, levels, [](daw::TrackKind) {});
    stretch(mixer);
    for (int i = 0; i < 4; ++i) {
        context.update();
        context.buildFrame();
    }
    daw::ChannelStrip *strip = findIn<daw::ChannelStrip>(mixer);
    daw::SmallToggle *toggle = strip ? findIn<daw::SmallToggle>(*strip) : nullptr;
    const bool inside = strip && toggle && toggle->rect().y + toggle->rect().h <= strip->rect().y + strip->rect().h + 0.5f &&
                        strip->rect().y + strip->rect().h <= 400.0f;
    c.check(inside && sendBarsIn(mixer) >= 12,
            "Flexible routing: a strip with 12 send bars keeps its M/S/R toggles inside the strip (the bars scroll)",
            strip && toggle ? "toggle bottom " + std::to_string(toggle->rect().y + toggle->rect().h) + ", strip bottom " +
                                  std::to_string(strip->rect().y + strip->rect().h)
                            : "no strip");
}

// Dragging a fader in a 64-track project moves the store's revision every frame: the strips must not be made again
// (the groups tracks send to are the same), and checking costs next to nothing
void faderDragChecks(Checker &c)
{
    using Clock = std::chrono::steady_clock;
    env::Environment environment;
    audio::AudioSystem engine(environment, audio::Deviceless{});
    engine::Window window(1500, 400, "VYBN fader drag test", false);
    window.setPosition(-32000, -32000);
    engine::Renderer renderer(window, false, true);
    const ui::Theme theme = daw::studioTheme(true);
    daw::Project p;
    for (int i = 0; i < 56; ++i)
        p.addTrack(daw::TrackKind::Audio);
    for (int i = 0; i < 4; ++i)
        p.addTrack(daw::TrackKind::Return);
    for (int i = 0; i < 4; ++i)
        p.addTrack(daw::TrackKind::Group);
    daw::ObjectId next = 1;
    p.normalizeIds(next);
    for (int i = 0; i < 56; ++i) {
        p.tracks[static_cast<size_t>(i)].sends = {{p.tracks[56].id, 0.5f, false}, {p.tracks[60 + i % 4].id, 1.0f, false}};
        p.tracks[static_cast<size_t>(i)].output = p.tracks[60 + (i + 1) % 4].id;
    }
    daw::ProjectStore store(p);
    const std::vector<float> levels(64, 0.0f);
    const daw::StereoLevels stereo{};
    ui::Context context(window, renderer, theme);
    auto &mixer = context.root().add<daw::MixerView>(context, store, engine, stereo, levels, [](daw::TrackKind) {});
    stretch(mixer);
    auto frame = [&] {
        context.update();
        context.buildFrame();
    };
    for (int i = 0; i < 4; ++i)
        frame();
    daw::ChannelStrip *before = findIn<daw::ChannelStrip>(mixer);
    constexpr int kFrames = 100;
    auto start = Clock::now();
    for (int i = 0; i < kFrames; ++i)
        frame();
    const double still = std::chrono::duration<double, std::milli>(Clock::now() - start).count() / kFrames;
    const uint64_t revision = store.revision();
    start = Clock::now();
    for (int i = 0; i < kFrames; ++i) {
        store.editMix(5).gain = 0.5f + 0.004f * static_cast<float>(i);
        frame();
    }
    const double dragging = std::chrono::duration<double, std::milli>(Clock::now() - start).count() / kFrames;
    start = Clock::now();
    size_t sink = 0;
    for (int i = 0; i < 1000; ++i)
        sink += daw::groupSendShape(store.project()).size();
    const double shapeUs = std::chrono::duration<double, std::micro>(Clock::now() - start).count() / 1000.0;
    c.check(findIn<daw::ChannelStrip>(mixer) == before && store.revision() >= revision + kFrames && sink > 0,
            "Flexible routing: dragging a fader in a 64-track project (the revision moving every frame) doesn't make the strips again",
            "");
    std::printf("INFO fader drag, 64 tracks: %.2f ms a frame still, %.2f ms dragging; groupSendShape %.1f us (%zu sends into groups)\n",
                still, dragging, shapeUs, sink / 1000);
}

// A project for the sidechain controls: a Pad audio track with a Compressor, a Kick, a Bus group
daw::Project sidechainProject()
{
    daw::Project p;
    p.tracks.resize(3);
    p.tracks[0].name = "Pad";
    p.tracks[1].name = "Kick";
    p.tracks[2].name = "Bus";
    p.tracks[2].kind = daw::TrackKind::Group;
    p.tracks[0].effects = {audio::effectSetting(audio::EffectType::Compressor), audio::effectSetting(audio::EffectType::Equalizer)};
    return p;
}

// Dragging an instrument out of the browser, by input handed to it as above: a press on an instrument's
// row and a move past a few pixels starts the drag once, with the row's key (a smaller move, or a
// click, starts none); and the arrangement above it finds the rows a drop lands on (a track, the space
// under the tracks for a new one). What joins the two (the ghost, the drop) is DawApp's.
void browserDragChecks(Checker &c)
{
    const std::filesystem::path temp = std::filesystem::temp_directory_path() / "vybn-browser-drag";
    std::error_code ec;
    std::filesystem::create_directories(temp, ec);
    env::Environment environment;
    environment.add<env::Scheduler>(env::SchedulerConfig{});
    daw::Settings settings;
    settings.libraryPath = (temp / "Library").u8string();
    daw::InstrumentLibrary library(environment, settings);
    daw::DeviceCatalogue catalogue(library, nullptr);
    daw::OnlineCatalogue online(environment, settings);
    daw::Downloads downloads(environment, library);

    engine::Window window(1200, 1000, "VYBN browser drag test", false);
    window.setPosition(-32000, -32000);
    engine::Renderer renderer(window, false, true);
    const ui::Theme theme = daw::studioTheme(true);
    daw::ProjectStore store(sidechainProject());
    ui::Context context(window, renderer, theme);
    auto &view = context.root().add<daw::ArrangementView>(store, quietTransport());
    view.showTiming(false);
    stretch(view);

    std::vector<std::string> dragged;
    int used = 0;
    daw::BrowserHooks hooks;
    hooks.catalogue = &catalogue;
    hooks.dragInstrument = [&](const std::string &key) { dragged.push_back(key); };
    hooks.useInstrument = [&](const std::string &) { ++used; };
    daw::AssetBrowser &browser = daw::AssetBrowser::open(context, library, online, downloads, hooks);
    browser.show();
    for (int i = 0; i < 3; ++i) {
        context.update();
        context.buildFrame();
    }

    int fb = 0, fbH = 0;
    window.framebufferSize(fb, fbH);
    const ui::Rect whole{0.0f, 0.0f, static_cast<float>(fb), static_cast<float>(fbH)};
    double now = 1.0;
    auto send = [&](ui::Vec2 at, bool down, bool pressed, bool released) {
        ui::Input in;
        in.mouse = at;
        in.down = down;
        in.pressed = pressed;
        in.released = released;
        in.dt = 0.1f;
        in.time = now += 0.5; // never two clicks close enough to be a double-click
        browser.handleInput(in);
        browser.arrange(whole, theme);
    };
    for (int i = 0; i < 12; ++i) // slid fully up, its rows laid out
        send({0.0f, 0.0f}, false, false, false);

    // A built-in instrument's row
    std::string key;
    ui::Rect row;
    for (const daw::CatalogueEntry &e : catalogue.entries()) {
        if (!e.instrument || e.plugin())
            continue;
        browser.chooseInstrument(e.key());
        send({0.0f, 0.0f}, false, false, false);
        row = browser.rowRect(e.key());
        if (row.w > 0.0f && browser.rect().contains(row.center())) {
            key = e.key();
            break;
        }
    }
    c.check(!key.empty() && browser.openness() == 1.0f, "Browser drag: the browser is open and a built-in instrument's row is shown", key);
    if (key.empty())
        return;

    const ui::Vec2 at = row.center();
    send(at, true, true, false);
    send({at.x + 3.0f, at.y - 3.0f}, true, false, false);
    const bool small = dragged.empty();
    send({at.x + 10.0f, at.y - 40.0f}, true, false, false);
    send({at.x + 20.0f, at.y - 200.0f}, true, false, false);
    send({at.x + 20.0f, at.y - 200.0f}, false, false, true);
    c.check(small && dragged.size() == 1 && dragged[0] == key, "Browser drag: moving a pressed instrument's row past a few pixels drags it, once, by its key",
            std::to_string(dragged.size()) + " drags");
    dragged.clear();
    send(at, true, true, false);
    send(at, false, false, true);
    c.check(dragged.empty() && used == 0, "Browser drag: a click on the row drags nothing (and uses nothing)");

    // Where it is dropped: on a track, or under the tracks (a new one); the browser covers the rest
    const daw::TrackHeader *first = view.header(0);
    const daw::TrackHeader *last = view.header(2);
    const float laneX = view.rect().x + view.rect().w * 0.6f;
    const int onTrack = first ? view.trackAt({laneX, first->rect().center().y}) : -2;
    const int under = last ? view.trackAt({laneX, last->rect().y + last->rect().h + 20.0f}) : -2;
    c.check(onTrack == 0 && under == 3 && last && last->rect().y + last->rect().h + 20.0f < browser.rect().y,
            "Browser drag: the arrangement finds a drop's track, and the space under the tracks for a new one",
            std::to_string(onTrack) + ", " + std::to_string(under));
    std::filesystem::remove_all(temp, ec);
}

// The Sidechain control on a compressor's card in the Devices panel (and in a device's own view): the
// choices (None and the tracks it may take, never itself), the one in use selected, a choice making
// an edit (one undo step), renaming, adding and deleting tracks showing at once, the "keyed by"
// caption, and the limit explained
void sidechainUiChecks(Checker &c)
{
    env::Environment environment;
    environment.add<env::Scheduler>(env::SchedulerConfig{});
    daw::Settings settings;
    settings.libraryPath = (std::filesystem::temp_directory_path() / "vybn-sidechain-ui").u8string();
    std::filesystem::create_directories(std::filesystem::u8path(settings.libraryPath));
    daw::InstrumentLibrary library(environment, settings);
    daw::DeviceCatalogue catalogue(library, nullptr);

    engine::Window window(1100, 500, "VYBN sidechain test", false);
    window.setPosition(-32000, -32000);
    engine::Renderer renderer(window, false, true);
    const ui::Theme theme = daw::studioTheme(true);
    daw::ProjectStore store(sidechainProject());
    store.select(0);
    ui::Context context(window, renderer, theme);
    // The audio side's devices, made on demand for whatever store a view shows
    std::map<std::string, std::unique_ptr<audio::BuiltinEffect>> devices;
    daw::ProjectStore *shown = &store;
    daw::DevicesHooks hooks;
    hooks.device = [&devices, &shown](const daw::DevicePlace &place) -> audio::Device * {
        const audio::DeviceSetting *setting = shown->project().device(place);
        audio::EffectType type;
        if (!setting || setting->kind != audio::DeviceKind::Effect || !audio::effectFromKey(setting->id, type))
            return nullptr;
        auto &made = devices[place.key() + setting->id];
        if (!made)
            made = audio::makeEffect(type);
        return made.get();
    };
    auto &view = context.root().add<daw::DevicesView>(context, store, catalogue, hooks);
    stretch(view);
    auto settle = [&] {
        for (int i = 0; i < 8; ++i) {
            context.update();
            context.buildFrame();
        }
    };
    settle();
    auto dropdown = [&]() -> ui::Dropdown * { return findIn<ui::Dropdown>(view); };
    auto captions = [&] {
        std::string text;
        labelsIn(view, text);
        return text;
    };
    auto optionsText = [&](ui::Dropdown *d) {
        std::string s;
        for (const std::string &o : d->options())
            s += (s.empty() ? "" : ", ") + o;
        return s;
    };
    ui::Dropdown *d = dropdown();
    c.check(d && d->options() == std::vector<std::string>{"None", "Kick", "Bus"} && d->selected() == 0 &&
                captions().find("Sidechain") != std::string::npos && captions().find("Keyed by") == std::string::npos,
            "Sidechain UI: a compressor's card offers None and the other tracks, nothing selected", d ? optionsText(d) : "(none)");

    if (!d) {
        std::string text;
        labelsIn(view, text);
        std::printf("INFO sidechain UI: no dropdown; %zu children in the view; labels: %s\n", view.childCount(), text.c_str());
        return;
    }
    // The EQ has none: one dropdown only (the compressor's)
    c.check(d && findIn<ui::Dropdown>(view) == d, "Sidechain UI: an EQ's card has no sidechain control");

    // A choice edits the device: one undo step, "Sidechain"
    daw::History history;
    history.reset(store.project());
    if (d)
        d->choose(1);
    history.update(store, true, false, 1.0);
    const daw::ObjectId kick = store.project().tracks[1].id;
    c.check(store.project().tracks[0].effects[0].sidechain == kick && history.undoLabel() == "Sidechain",
            "Sidechain UI: choosing a track keys the effect, as one undo step named \"Sidechain\"", history.undoLabel());
    settle();
    d = dropdown();
    c.check(d && d->selected() == 1 && captions().find("Keyed by Kick") != std::string::npos,
            "Sidechain UI: the card says what keys it", captions());

    // Renaming the source updates the choices and the caption
    store.editTrack(1).name = "Bass";
    settle();
    d = dropdown();
    c.check(d && d->options() == std::vector<std::string>{"None", "Bass", "Bus"} && d->selected() == 1 &&
                captions().find("Keyed by Bass") != std::string::npos,
            "Sidechain UI: renaming the source updates the dropdown and the caption", d ? optionsText(d) : "(none)");

    // A new track appears in the choices; one the pad feeds doesn't (a loop)
    store.edit().addTrack(daw::TrackKind::Audio);
    store.normalizeIds(); // as the audio bridge does each frame
    store.select(0);
    settle();
    d = dropdown();
    c.check(d && d->options().size() == 4 && d->selected() == 1, "Sidechain UI: a track added shows in the choices", d ? optionsText(d) : "(none)");
    store.editMix(0).output = store.project().tracks[2].id; // Pad into Bus
    settle();
    d = dropdown();
    c.check(d && d->options().size() == 3 && d->options()[1] == "Bass" && d->options()[2] != "Bus",
            "Sidechain UI: a track the pad feeds can't key it", d ? optionsText(d) : "(none)");

    // None unkeys; deleting the source clears the key (and undo brings both back)
    if (d)
        d->choose(0);
    settle();
    c.check(store.project().tracks[0].effects[0].sidechain == daw::kNoId && captions().find("Keyed by") == std::string::npos,
            "Sidechain UI: None removes the key and the caption");
    d = dropdown();
    store.editMix(0).output = daw::kNoId;
    if (d)
        d->choose(1);
    settle();
    c.check(store.project().tracks[0].effects[0].sidechain == kick, "Sidechain UI: and a track keys it again");
    daw::History deleting;
    deleting.reset(store.project());
    store.edit().deleteTrack(1);
    deleting.update(store, true, false, 1.0);
    store.select(0);
    settle();
    d = dropdown();
    c.check(store.project().tracks[0].effects[0].sidechain == daw::kNoId && d && d->selected() == 0 &&
                captions().find("Keyed by") == std::string::npos && deleting.undoLabel() == "Delete track",
            "Sidechain UI: deleting the source clears the key and the caption",
            std::to_string(store.project().tracks[0].effects[0].sidechain) + ", selected " +
                std::to_string(d ? d->selected() : -9) + " of " + (d ? optionsText(d) : std::string("-")) + ", captions [" + captions() + "], label " + deleting.undoLabel());
    daw::HistoryRestore plan;
    const bool undone = deleting.undo(store, 3.0, plan);
    store.restore(std::move(plan));
    store.select(0);
    settle();
    d = dropdown();
    c.check(undone && store.project().tracks.size() == 4 && store.project().tracks[0].effects[0].sidechain == kick && d &&
                d->selected() == 1 && captions().find("Keyed by Bass") != std::string::npos,
            "Sidechain UI: one undo brings the track and the key back together");

    // The master's effects may take any track
    {
        daw::Project p = sidechainProject();
        p.masterEffects = {audio::effectSetting(audio::EffectType::Gate)};
        daw::ProjectStore masterStore(p);
        shown = &masterStore;
        masterStore.select(0);
        ui::Context masterContext(window, renderer, theme);
        auto &master = masterContext.root().add<daw::DevicesView>(masterContext, masterStore, catalogue, hooks);
        master.setMaster(true);
        stretch(master);
        for (int i = 0; i < 8; ++i) {
            masterContext.update();
            masterContext.buildFrame();
        }
        ui::Dropdown *m = findIn<ui::Dropdown>(master);
        c.check(m && m->options() == std::vector<std::string>{"None", "Pad", "Kick", "Bus"},
                "Sidechain UI: the master's gate may be keyed by any track", m ? optionsText(m) : "(none)");
    }

    // The limit: four keyed effects on a track, and the fifth explains why it offers none
    {
        daw::Project p = sidechainProject();
        p.tracks[0].effects.clear();
        for (int i = 0; i < 5; ++i)
            p.tracks[0].effects.push_back(audio::effectSetting(audio::EffectType::Compressor));
        daw::ProjectStore limitStore(p);
        shown = &limitStore;
        for (int i = 0; i < 4; ++i)
            limitStore.editTrack(0).effects[static_cast<size_t>(i)].sidechain = limitStore.project().tracks[1].id;
        limitStore.select(0);
        ui::Context limitContext(window, renderer, theme);
        auto &limited = limitContext.root().add<daw::DevicesView>(limitContext, limitStore, catalogue, hooks);
        stretch(limited);
        for (int i = 0; i < 8; ++i) {
            limitContext.update();
            limitContext.buildFrame();
        }
        std::string text;
        labelsIn(limited, text);
        const daw::SidechainOffer fifth = daw::sidechainOffer(limitStore.project(), daw::DevicePlace::effect(false, 0, 4));
        const daw::SidechainOffer first = daw::sidechainOffer(limitStore.project(), daw::DevicePlace::effect(false, 0, 0));
        c.check(fifth.available && fifth.sources.empty() && fifth.note == "4 sidechains per track at most" && !first.sources.empty() &&
                    first.note.empty() && text.find("4 sidechains per track at most") != std::string::npos,
                "Sidechain UI: with four keyed effects the fifth offers none and says why", fifth.note);
    }
}

// The Devices panel on a pad: a compressor keyed by the Kick track (its card says "Keyed by Kick"), an EQ (no
// sidechain row) and a gate with none
Picture renderSidechain(ui::FrameStats *after)
{
    daw::Project project = sidechainProject();
    project.tracks[0].effects.push_back(audio::effectSetting(audio::EffectType::Gate));
    daw::ProjectStore store(project);
    store.editTrack(0).effects[0].sidechain = store.project().tracks[1].id;
    store.select(0);
    std::map<std::string, std::unique_ptr<audio::BuiltinEffect>> devices;
    daw::ProjectStore *shown = &store;
    daw::DevicesHooks hooks;
    hooks.device = [&devices, &shown](const daw::DevicePlace &place) -> audio::Device * {
        const audio::DeviceSetting *setting = shown->project().device(place);
        audio::EffectType type;
        if (!setting || setting->kind != audio::DeviceKind::Effect || !audio::effectFromKey(setting->id, type))
            return nullptr;
        auto &made = devices[place.key() + setting->id];
        if (!made)
            made = audio::makeEffect(type);
        return made.get();
    };
    env::Environment environment;
    environment.add<env::Scheduler>(env::SchedulerConfig{});
    daw::Settings settings;
    settings.libraryPath = (std::filesystem::temp_directory_path() / "vybn-sidechain-ui").u8string();
    daw::InstrumentLibrary library(environment, settings);
    daw::DeviceCatalogue catalogue(library, nullptr);
    return renderPicture(1760, 300, daw::studioTheme(true), [&](ui::Context &context) {
        stretch(context.root().add<daw::DevicesView>(context, store, catalogue, hooks));
    }, 0, nullptr, after);
}

Picture sceneSidechain()
{
    return renderSidechain(nullptr);
}

// The Devices panel on a pad with two of the project's test plugins loaded in their sandboxes: Test Duck
// (a sidechain port, so a Sidechain row) keyed by the kick, and Test Gain (no port, no row). The plugins
// are real: a manager scans output/plugins and a chain loads them, and the picture is taken only once both
// are ready, so it waits on the hosts starting but never races them. Without the test plugins it fails.
Picture renderPluginSidechain(ui::FrameStats *after)
{
    using Clock = std::chrono::steady_clock;
    env::Environment environment;
    environment.add<env::Scheduler>(env::SchedulerConfig{});
    plugin::PluginConfig config;
    config.folders = {std::filesystem::u8path(ENGINE_TEST_PLUGIN).parent_path().u8string()};
    plugin::PluginManager manager(environment, config);
    manager.rescan();
    const auto scanned = Clock::now() + std::chrono::seconds(20);
    while (manager.scanning() && Clock::now() < scanned)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    daw::setPluginSidechainLookup([&manager](const std::string &id) { return manager.sidechainChannels(id); });

    daw::Project project = sidechainProject();
    audio::DeviceSetting duck;
    duck.kind = audio::DeviceKind::Plugin;
    duck.id = "vybn.test.duck";
    duck.name = "Test Duck";
    audio::DeviceSetting gain = duck;
    gain.id = "vybn.test.gain";
    gain.name = "Test Gain";
    project.tracks[0].effects = {duck, gain};
    daw::ProjectStore store(project);
    store.editTrack(0).effects[0].sidechain = store.project().tracks[1].id;
    store.select(0);

    // The pad's chain, loaded until both plugins are ready (or failed: the picture shows why)
    audio::DeviceChain chain;
    chain.setPlugins(&manager);
    chain.prepare(48000.0, 512);
    chain.apply(store.project().tracks[0].effects);
    const auto loaded = Clock::now() + std::chrono::seconds(20);
    auto loading = [&chain] {
        for (size_t i = 0; i < chain.size(); ++i) {
            plugin::Slot *slot = chain.effect(i) ? chain.effect(i)->slot() : nullptr;
            if (!slot || slot->state() == plugin::Slot::State::Loading)
                return true;
        }
        return false;
    };
    while (loading() && Clock::now() < loaded) {
        chain.collect();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    daw::DevicesHooks hooks;
    hooks.device = [&chain, &store](const daw::DevicePlace &place) -> audio::Device * {
        const audio::DeviceSetting *setting = store.project().device(place);
        audio::Device *device = !place.master && place.track == 0 && place.index >= 0 ? chain.effect(static_cast<size_t>(place.index)) : nullptr;
        return device && setting && device->matches(*setting) ? device : nullptr;
    };
    daw::Settings settings;
    settings.libraryPath = (std::filesystem::temp_directory_path() / "vybn-sidechain-ui").u8string();
    daw::InstrumentLibrary library(environment, settings);
    daw::DeviceCatalogue catalogue(library, &manager);
    Picture picture = renderPicture(1000, 300, daw::studioTheme(true), [&](ui::Context &context) {
        stretch(context.root().add<daw::DevicesView>(context, store, catalogue, hooks));
    }, 0, nullptr, after);
    daw::setPluginSidechainLookup(nullptr);
    return picture;
}

Picture scenePluginSidechain()
{
    return renderPluginSidechain(nullptr);
}

// A benchmark (VYBN_TEST_BENCH): what the Sidechain controls cost a still frame on a big project, 64 tracks
// in 4 groups and a pad with 8 compressors (3 keyed): the offers of its 8 cards worked out afresh (done only when the project
// changes now (22 ms a frame before: the controls worked them out every frame, and with the routes made afresh per
// track), and settled frames of the Devices panel
void sidechainBench()
{
    using Clock = std::chrono::steady_clock;
    daw::Project project;
    project.tracks.resize(64);
    for (size_t i = 0; i < project.tracks.size(); ++i) {
        project.tracks[i].name = "Track " + std::to_string(i + 1);
        project.tracks[i].kind = i >= 60 ? daw::TrackKind::Group : daw::TrackKind::Audio;
    }
    project.tracks[0].effects.assign(8, audio::effectSetting(audio::EffectType::Compressor));
    daw::ProjectStore store(project);
    for (size_t i = 0; i < 60; ++i)
        store.editMix(static_cast<int>(i)).output = store.project().tracks[60 + i % 4].id;
    for (size_t i = 0; i < 3; ++i)
        store.editTrack(0).effects[i].sidechain = store.project().tracks[1].id;
    store.select(0);

    constexpr int kFrames = 200;
    size_t sources = 0;
    auto start = Clock::now();
    for (int frame = 0; frame < kFrames; ++frame)
        for (int e = 0; e < 8; ++e)
            sources += daw::sidechainOffer(store.project(), daw::DevicePlace::effect(false, 0, e)).sources.size();
    const double offers = std::chrono::duration<double, std::milli>(Clock::now() - start).count() / kFrames;

    env::Environment environment;
    environment.add<env::Scheduler>(env::SchedulerConfig{});
    daw::Settings settings;
    settings.libraryPath = (std::filesystem::temp_directory_path() / "vybn-sidechain-ui").u8string();
    daw::InstrumentLibrary library(environment, settings);
    daw::DeviceCatalogue catalogue(library, nullptr);
    std::map<std::string, std::unique_ptr<audio::BuiltinEffect>> devices;
    daw::DevicesHooks hooks;
    hooks.device = [&devices, &store](const daw::DevicePlace &place) -> audio::Device * {
        const audio::DeviceSetting *setting = store.project().device(place);
        audio::EffectType type;
        if (!setting || setting->kind != audio::DeviceKind::Effect || !audio::effectFromKey(setting->id, type))
            return nullptr;
        auto &made = devices[place.key() + setting->id];
        if (!made)
            made = audio::makeEffect(type);
        return made.get();
    };
    engine::Window window(1800, 400, "VYBN sidechain bench", false);
    window.setPosition(-32000, -32000);
    engine::Renderer renderer(window, false, true);
    ui::Context context(window, renderer, daw::studioTheme(true));
    stretch(context.root().add<daw::DevicesView>(context, store, catalogue, hooks));
    for (int i = 0; i < 20; ++i) {
        context.update();
        context.buildFrame();
    }
    start = Clock::now();
    for (int frame = 0; frame < kFrames; ++frame) {
        context.update();
        context.buildFrame();
    }
    const double frames = std::chrono::duration<double, std::milli>(Clock::now() - start).count() / kFrames;
    std::printf("INFO sidechain bench: 64 tracks, 8 cards on a pad (3 keyed): offers of the 8 cards worked out %.3f ms (%zu sources; only when the project changes); "
                "a settled Devices frame %.3f ms\n",
                offers, sources / (kFrames * 8), frames);
}

// A device's own view has the same row
void sidechainDeviceViewChecks(Checker &c)
{
    engine::Window window(900, 500, "VYBN sidechain device view test", false);
    window.setPosition(-32000, -32000);
    engine::Renderer renderer(window, false, true);
    const ui::Theme theme = daw::studioTheme(true);
    daw::ProjectStore store(sidechainProject());
    ui::Context context(window, renderer, theme);
    const daw::DevicePlace place = daw::DevicePlace::effect(false, 0, 0);
    daw::DeviceAccess access;
    access.setting = [&store, place] { return store.project().device(place); };
    access.edit = [&store, place] { return store.editDevice(place); };
    access.device = [] { return static_cast<audio::Device *>(nullptr); };
    access.sidechain = [&store, place] { return daw::sidechainOffer(store.project(), place); };
    auto &view = context.root().add<daw::DeviceView>(context, access, "Compressor", "Built-in effect", true);
    stretch(view);
    for (int i = 0; i < 8; ++i) {
        context.update();
        context.buildFrame();
    }
    ui::Dropdown *d = findIn<ui::Dropdown>(view);
    c.check(d && d->options() == std::vector<std::string>{"None", "Kick", "Bus"}, "Sidechain UI: a device's own view has the sidechain row too");
    if (d)
        d->choose(2);
    c.check(store.project().tracks[0].effects[0].sidechain == store.project().tracks[2].id,
            "Sidechain UI: and choosing there edits the device");
}

// The track header's toggles as a radial menu (RadialToggles opening a RadialMenu), by input handed
// to the open menu: dragging onto a button and letting go flips it and closes; letting go on the
// middle keeps it open for clicks; the middle closes it; a press outside the disc isn't its own
void radialToggleChecks(Checker &c)
{
    engine::Window window(800, 600, "VYBN radial test", false);
    window.setPosition(-32000, -32000);
    engine::Renderer renderer(window, false, true);
    const ui::Theme theme = daw::studioTheme(true);
    bool mute = false, solo = false, arm = false;
    auto toggle = [](const char *label, const char *name, bool &value) {
        return daw::RadialToggle{label, name, engine::Key::Unknown, [&value] { return value; }, [&value](bool on) { value = on; }};
    };
    ui::Context context(window, renderer, theme);
    auto &hub = context.root().add<daw::RadialToggles>(
        std::vector<daw::RadialToggle>{toggle("M", "Mute", mute), toggle("S", "Solo", solo), toggle("R", "Record arm", arm)});
    for (int i = 0; i < 3; ++i) {
        context.update();
        context.buildFrame();
    }
    const ui::Vec2 centre = hub.rect().center();
    auto buttonAt = [&centre](size_t i) {
        const float a = daw::RadialToggles::angle(i, 3);
        return ui::Vec2{centre.x + std::cos(a) * daw::RadialMenu::kRing, centre.y + std::sin(a) * daw::RadialMenu::kRing};
    };
    int fb = 0, fbH = 0;
    window.framebufferSize(fb, fbH);
    const ui::Rect whole{0.0f, 0.0f, static_cast<float>(fb), static_cast<float>(fbH)};
    auto send = [&](daw::RadialMenu &menu, ui::Vec2 at, bool down, bool pressed, bool released) {
        ui::Input in;
        in.mouse = at;
        in.down = down;
        in.pressed = pressed;
        in.released = released;
        in.dt = 1.0f; // opens fully at once
        menu.arrange(whole, theme);
        menu.handleInput(in);
    };
    auto settle = [&] {
        context.update();
        context.buildFrame();
    };

    daw::RadialMenu &held = hub.openMenu(context, true);
    send(held, centre, true, false, false);
    send(held, buttonAt(1), true, false, false);
    send(held, buttonAt(1), false, false, true);
    settle();
    c.check(solo && !mute && !arm && !hub.menuOpen(), "Radial toggles: dragging onto Solo and letting go turns it on and closes");

    daw::RadialMenu &clicked = hub.openMenu(context, true);
    send(clicked, centre, true, false, false);
    send(clicked, centre, false, false, true); // let go on the middle: stays open
    send(clicked, buttonAt(0), true, true, false);
    send(clicked, buttonAt(0), false, false, true);
    send(clicked, buttonAt(2), true, true, false);
    send(clicked, buttonAt(2), false, false, true);
    send(clicked, buttonAt(1), true, true, false);
    send(clicked, buttonAt(1), false, false, true);
    settle();
    const bool stillOpen = hub.menuOpen();
    const bool outside = clicked.hitTest({centre.x + 200.0f, centre.y});
    send(clicked, centre, true, true, false); // the middle closes it
    settle();
    c.check(stillOpen && mute && arm && !solo && !outside && !hub.menuOpen(),
            "Radial toggles: let go on the middle, clicks flip buttons until the middle closes it");
}

// Holds its children still: no input reaches them, so wherever the real mouse is it hovers nothing
class StillContainer : public ui::Container {
public:
    StillContainer() : Container(ui::Direction::Column) {}
    void handleInput(const ui::Input &) override {}
};

// A track header's toggles opened as a radial menu: Mute, Record arm and Write automation on, Solo
// and the lanes off. The menu is in the tree under a StillContainer rather than an overlay (which
// takes the window's real mouse), opened fully by one frame of input handed to it
Picture sceneRadial()
{
    static bool values[5] = {true, false, true, false, true};
    const char *labels[5] = {"M", "S", "R", "A", "W"};
    const char *names[5] = {"Mute", "Solo", "Record arm", "Automation lanes", "Write automation"};
    return renderPicture(360, 260, daw::studioTheme(true), [&](ui::Context &context) {
        std::vector<daw::RadialToggle> toggles;
        for (int i = 0; i < 5; ++i) {
            bool *value = &values[i];
            toggles.push_back({labels[i], names[i], engine::Key::Unknown, [value] { return *value; }, [value](bool on) { *value = on; }});
        }
        int w = 0, h = 0;
        context.window().framebufferSize(w, h);
        const ui::Vec2 centre{static_cast<float>(w) * 0.5f, static_cast<float>(h) * 0.5f};
        auto &menu = context.root().add<StillContainer>().add<daw::RadialMenu>(context, std::move(toggles), centre, false);
        menu.arrange({0.0f, 0.0f, static_cast<float>(w), static_cast<float>(h)}, context.theme());
        ui::Input in;
        in.mouse = {-1000.0f, -1000.0f};
        in.dt = 1.0f; // fully open
        menu.handleInput(in);
    }, 0, nullptr);
}

// The arrangement with the first track's automation shown: a volume lane with a curve, and an
// empty pan lane (its resting value dashed, with a hint)
Picture sceneAutomation()
{
    daw::Project project = daw::demoProject();
    auto &lanes = project.tracks[0].automation;
    lanes.push_back({audio::kVolumeParam, {{0.0, 0.2f}, {8.0, 0.8f}, {16.0, 0.8f}, {20.0, 0.4f}, {28.0, 0.9f}}});
    lanes.push_back({audio::kPanParam, {}});
    daw::ProjectStore store(project); // outlives the context
    return renderPicture(1200, 520, daw::studioTheme(true), [&store](ui::Context &context) {
        auto &view = context.root().add<daw::ArrangementView>(store, quietTransport());
        view.showTiming(false); // the tempo lanes have their own scene
        view.showAutomation(0, true);
        stretch(view);
    }, 0, nullptr);
}

// The arrangement with its tempo and signature lanes: a ramp from 100 to 140 BPM over four bars, held,
// then a drop to 90; two bars of 7/8 from bar 5, then 6/8. The ruler and the grid follow the bars.
Picture sceneTempo()
{
    daw::Project project = daw::demoProject();
    project.timing.setPoints({{0.0, 100.0, true}, {16.0, 140.0, false}, {32.0, 90.0, false}});
    project.timing.setSignatures({{0, 4, 4}, {4, 7, 8}, {6, 6, 8}});
    project.markers = {{1, 0.0, "Intro"}, {2, 16.0, "Verse"}, {3, 32.0, "Chorus"}};
    daw::ProjectStore store(project); // outlives the context
    return renderPicture(1200, 520, daw::studioTheme(true), [&store](ui::Context &context) {
        stretch(context.root().add<daw::ArrangementView>(store, quietTransport()));
    }, 0, nullptr);
}

// The demo project with three MIDI takes on the Lead track (index 2) over two bars, newest first, the
// comp cut from all three: the third take's opening, the first's middle, the second's end
daw::Project takesProject()
{
    daw::Project project = daw::demoProject();
    daw::Track &lead = project.tracks[2];
    lead.clips.clear();
    const int melody[3][8] = {{69, 72, 76, 74, 72, 69, 67, 69}, {69, 71, 72, 76, 79, 76, 74, 72}, {64, 67, 69, 72, 71, 69, 67, 64}};
    for (int pass = 0; pass < 3; ++pass) {
        daw::Clip take("Take " + std::to_string(pass + 1), 0.0, 8.0);
        take.id = 900001 + static_cast<daw::ObjectId>(pass);
        for (int n = 0; n < 8; ++n)
            take.notes.push_back({static_cast<double>(n), 0.9, melody[pass][n], 0.8f});
        lead.takeLanes.insert(lead.takeLanes.begin(), daw::TakeLane{900011 + static_cast<daw::ObjectId>(pass), take.name, {take}});
    }
    daw::compRange(lead, 0.0, 2.0, 900003, project.timing);
    daw::compRange(lead, 2.0, 5.0, 900001, project.timing);
    daw::compRange(lead, 5.0, 8.0, 900002, project.timing);
    return project;
}

// With `after`, one more frame is run after the picture (the cache scenes)
Picture renderTakes(ui::FrameStats *after)
{
    daw::ProjectStore store(takesProject()); // outlives the context
    return renderPicture(1200, 520, daw::studioTheme(true), [&store](ui::Context &context) {
        auto &view = context.root().add<daw::ArrangementView>(store, quietTransport());
        view.showTiming(false);
        view.showTakes(2, true);
        stretch(view);
    }, 0, nullptr, after);
}

Picture sceneTakes()
{
    return renderTakes(nullptr);
}

// Comping in the take lanes, by input handed to the view as above: under the ruler (30), the Lead
// track's row is the third (76 each), its take lanes (52 each) under it, newest first.
void takeEditChecks(Checker &c)
{
    engine::Window window(1200, 520, "VYBN takes test", false);
    window.setPosition(-32000, -32000);
    engine::Renderer renderer(window, false, true);
    const ui::Theme theme = daw::studioTheme(true);
    daw::ProjectStore store(takesProject());
    daw::ObjectId heard = daw::kNoId, menuLane = daw::kNoId;
    daw::TransportHooks hooks = quietTransport();
    hooks.auditioned = [&heard](int) { return heard; };
    hooks.audition = [&heard](int, daw::ObjectId lane) { heard = lane; };
    hooks.takeMenu = [&menuLane](int, daw::ObjectId lane, daw::ObjectId, ui::Vec2) { menuLane = lane; };
    ui::Context context(window, renderer, theme);
    auto &view = context.root().add<daw::ArrangementView>(store, hooks);
    view.showTiming(false);
    stretch(view);
    view.showTakes(2, true);
    for (int i = 0; i < 4; ++i) {
        context.update();
        context.buildFrame();
    }
    const ui::Rect r = view.rect();
    const float wellX = r.x + 278.0f, lanesTop = r.y + 30.0f + 3 * 76.0f;
    auto beatX = [wellX](double beat) { return wellX + static_cast<float>(beat) * 22.0f; };
    auto laneY = [lanesTop](int lane) { return lanesTop + 52.0f * static_cast<float>(lane) + 26.0f; };
    double now = 10.0;
    auto send = [&](ui::Vec2 at, bool down, bool pressed, bool secondary = false) {
        ui::Input in;
        in.mouse = at;
        in.down = down;
        in.pressed = pressed;
        in.secondaryPressed = secondary;
        in.time = now;
        view.handleInput(in);
        view.arrange(view.rect(), theme);
        now += 0.5;
    };
    auto comp = [&store] {
        std::string text;
        for (const daw::Clip &clip : store.project().tracks[2].clips)
            text += std::to_string(clip.take % 10) + "@" + std::to_string(static_cast<int>(clip.start)) + "-" + std::to_string(static_cast<int>(clip.end())) + " ";
        return text;
    };

    // A click in the second lane (take 2) over beat 3 switches the section there (2-5) to it
    send({beatX(3.3), laneY(1)}, true, true);
    send({beatX(3.3), laneY(1)}, false, false);
    c.check(comp() == "3@0-2 2@2-8 ", "Take lanes: a click switches the section under it", comp());

    // A swipe across the bottom lane (take 1) from beat 1 to 4 comps that range from it
    send({beatX(1.0), laneY(2)}, true, true);
    send({beatX(2.5), laneY(2)}, true, false);
    send({beatX(4.0), laneY(2)}, true, false);
    send({beatX(4.0), laneY(2)}, false, false);
    c.check(comp() == "3@0-1 1@1-4 2@4-8 ", "Take lanes: a swipe comps the range it crosses", comp());

    // Swiping back while still held gives back what the swipe took
    send({beatX(6.0), laneY(0)}, true, true);
    send({beatX(7.5), laneY(0)}, true, false);
    send({beatX(6.5), laneY(0)}, true, false);
    send({beatX(6.5), laneY(0)}, false, false);
    c.check(comp() == "3@0-1 1@1-4 2@4-6 3@6-7 2@7-8 ", "Take lanes: swiping back gives the comp back", comp());

    // A double-click on the top take: all of it
    const double first = now;
    send({beatX(5.2), laneY(0)}, true, true);
    send({beatX(5.2), laneY(0)}, false, false);
    now = first + 0.2;
    send({beatX(5.2), laneY(0)}, true, true);
    send({beatX(5.2), laneY(0)}, false, false);
    c.check(comp() == "3@0-8 ", "Take lanes: a double-click uses the whole take", comp());

    // A boundary drags: comp 2-4 from take 1, then pull its left edge to beat 3
    send({beatX(2.0), laneY(2)}, true, true);
    send({beatX(4.0), laneY(2)}, true, false);
    send({beatX(4.0), laneY(2)}, false, false);
    send({beatX(2.0), laneY(1)}, true, true);
    send({beatX(3.0), laneY(1)}, true, false);
    send({beatX(3.0), laneY(1)}, false, false);
    c.check(comp() == "3@0-3 1@3-4 3@4-8 ", "Take lanes: a boundary moves when dragged", comp());

    // The header: listen to a lane, use it all, its menu, delete it
    const float buttonY = laneY(1);
    const float headerRight = r.x + 268.0f;
    send({headerRight - 20.0f - 52.0f, buttonY}, true, true); // listen
    send({headerRight - 20.0f - 52.0f, buttonY}, false, false);
    const bool listening = heard == 900012;
    send({headerRight - 20.0f - 26.0f, buttonY}, true, true); // use
    send({headerRight - 20.0f - 26.0f, buttonY}, false, false);
    const bool used = comp() == "2@0-8 ";
    send({r.x + 100.0f, buttonY}, false, false, true); // menu
    send({headerRight - 20.0f, buttonY}, true, true); // delete
    send({headerRight - 20.0f, buttonY}, false, false);
    const daw::Track &lead = store.project().tracks[2];
    c.check(listening && used && menuLane == 900012 && lead.takeLanes.size() == 2 && lead.clips[0].take == daw::kNoId && heard == daw::kNoId,
            "Take lanes: a lane's header listens, uses, opens its menu and deletes", comp());
}

// The same song with the lanes hidden: the ruler marks the signature changes by their bars' numbers
// and each tempo change with a dot, and the corner chip offers the lanes
Picture sceneTempoHidden()
{
    daw::Project project = daw::demoProject();
    project.timing.setPoints({{0.0, 100.0, true}, {16.0, 140.0, false}, {32.0, 90.0, false}});
    project.timing.setSignatures({{0, 4, 4}, {4, 7, 8}, {6, 6, 8}});
    project.markers = {{1, 0.0, "Intro"}, {2, 16.0, "Verse"}, {3, 32.0, "Chorus"}};
    daw::ProjectStore store(project); // outlives the context
    return renderPicture(1200, 520, daw::studioTheme(true), [&store](ui::Context &context) {
        auto &view = context.root().add<daw::ArrangementView>(store, quietTransport());
        view.showTiming(false);
        stretch(view);
    }, 0, nullptr);
}

// The arrangement with the master's lanes shown under the tracks: its volume lane with a dip, and a
// Gain effect on the master with a lane for its level (read by name through a deviceParams hook, as the
// app does). With `after`, one more frame is run after the picture (see settledCacheChecks).
Picture renderMaster(ui::FrameStats *after)
{
    daw::Project project = daw::demoProject();
    project.masterEffects.push_back(audio::effectSetting(audio::EffectType::Gain));
    project.masterEffects[0].automation.push_back({0, {{0.0, 0.3f}, {12.0, 0.7f}, {24.0, 0.5f}}});
    project.masterAutomation.push_back({audio::kVolumeParam, {{0.0, 0.79f}, {8.0, 0.79f}, {12.0, 0.4f}, {24.0, 0.79f}}});
    daw::ProjectStore store(project); // outlives the context
    daw::TransportHooks hooks = quietTransport();
    hooks.deviceParams = [](const daw::DevicePlace &) {
        std::vector<audio::DeviceParam> params;
        const audio::EffectInfo &info = audio::effectInfo(audio::EffectType::Gain);
        for (uint32_t i = 0; i < info.paramCount; ++i)
            params.push_back(audio::effectParam(info.params[i], i));
        return params;
    };
    return renderPicture(1200, 740, daw::studioTheme(true), [&store, &hooks](ui::Context &context) {
        auto &view = context.root().add<daw::ArrangementView>(store, hooks);
        view.showTiming(false); // the tempo lanes have their own scene
        view.showMasterAutomation(true);
        stretch(view);
    }, 0, nullptr, after);
}

Picture sceneMaster()
{
    return renderMaster(nullptr);
}

// The mixer with routing: the demo's tracks with two returns and a group added. The guitar sends to
// the first return after its fader, the bass to it before its fader and to the second after; the drums and vocals go into the group; the master's
// volume is down a little. Played by a deviceless engine (so the master strip says "No output").
Picture sceneMixer()
{
    static env::Environment environment;
    static audio::AudioSystem engine(environment, audio::Deviceless{});
    daw::Project project = daw::demoProject();
    project.addTrack(daw::TrackKind::Return);
    project.addTrack(daw::TrackKind::Return);
    project.addTrack(daw::TrackKind::Group);
    project.addTrack(daw::TrackKind::Group);
    daw::ObjectId next = 1;
    project.normalizeIds(next);
    const daw::ObjectId ret = project.tracks[6].id, ret2 = project.tracks[7].id, group = project.tracks[8].id;
    project.tracks[0].sends = {{ret, 0.5f, false}, {project.tracks[9].id, 0.7f, false}}; // and also to Group 2
    project.tracks[0].output = group;
    project.tracks[1].sends = {{ret, 0.3f, true}, {ret2, 0.6f, false}};
    project.tracks[3].output = group;
    project.tracks[4].output = group;
    project.masterVolume = 0.8f;
    project.select(0);
    daw::ProjectStore store(project); // outlives the context
    static const std::vector<float> levels(10, 0.0f);
    static const daw::StereoLevels stereo{};
    return renderPicture(1500, 400, daw::studioTheme(true), [&store](ui::Context &context) {
        stretch(context.root().add<daw::MixerView>(context, store, engine, stereo, levels, [](daw::TrackKind) {}));
    }, 0, nullptr);
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
        auto &view = context.root().add<daw::ArrangementView>(store, hooks);
        view.showTiming(false); // the tempo lanes have their own scene
        stretch(view);
    }, timedFrames, timing, after);
}

Picture sceneWaveforms()
{
    return renderWaveforms(0, nullptr);
}

// Warping (docs/design/warp-plan.md): a drum loop at 100 BPM warped onto a 120 BPM song in Beats,
// transposed up three semitones, one marker moved so its bar stretches, over the same clip unwarped;
// below, the audio editor on the warped clip with its markers, its transients and the file around it
Picture sceneWarp()
{
    // Hits every 0.6 s (100 BPM), loud on the beat, softer between
    const double rate = 48000.0;
    std::vector<float> samples(static_cast<size_t>(4.8 * rate));
    uint32_t state = 12345u;
    for (size_t i = 0; i < samples.size(); ++i) {
        const double inBeat = std::fmod(static_cast<double>(i) / rate, 0.3);
        const bool strong = static_cast<size_t>(static_cast<double>(i) / rate / 0.3) % 2 == 0;
        state = state * 1664525u + 1013904223u;
        const float noise = static_cast<float>(state >> 8) / 8388608.0f - 1.0f;
        samples[i] = noise * static_cast<float>(std::exp(-inBeat * (strong ? 14.0 : 30.0)) * (strong ? 0.8 : 0.35));
    }
    auto sound = audio::makeAudioData("loop", rate, 1, samples);
    const auto analysis = std::make_shared<audio::AudioAnalysis>(audio::analyseSamples(samples.data(), samples.size(), 1, rate));

    daw::Project project;
    project.name = "Warp";
    daw::Track warpedTrack;
    warpedTrack.name = "Loop (warped)";
    daw::Clip clip("Drum loop", 0.0, 8.0);
    clip.id = 1;
    clip.audioFile = "loop";
    clip.warp.on = true;
    clip.warp.bpm = 100.0;
    clip.warp.mode = audio::WarpMode::Beats;
    clip.warp.pitch = 3.0;
    clip.warp.markers = {{0.0, 0.0}, {1.2, 2.0}, {2.4, 4.0}};
    clip.length = 8.0;
    daw::moveWarpMarker(clip, 1, 2.5);
    warpedTrack.clips.push_back(clip);
    project.tracks.push_back(warpedTrack);
    daw::Track plainTrack;
    plainTrack.name = "Loop (unwarped)";
    daw::Clip plain("Drum loop", 0.0, project.beatsFor(0.0, sound->seconds()));
    plain.id = 2;
    plain.audioFile = "loop";
    plainTrack.clips.push_back(plain);
    project.tracks.push_back(plainTrack);

    daw::TransportHooks hooks = quietTransport();
    hooks.audioFor = [&sound](int, int) -> const audio::AudioData * { return sound.get(); };
    daw::AudioEditorHooks editorHooks;
    editorHooks.audioFor = [&sound](const daw::Clip &) -> const audio::AudioData * { return sound.get(); };
    editorHooks.analysisFor = [&analysis](const daw::Clip &) -> const audio::AudioAnalysis * { return analysis.get(); };
    editorHooks.position = [] { return 0.0; };
    editorHooks.seek = [](double) {};
    daw::ProjectStore store(std::move(project));
    store.select(0, 0);
    return renderPicture(1200, 600, daw::studioTheme(true), [&store, &hooks, &editorHooks](ui::Context &context) {
        auto &column = context.root().add<ui::Container>(ui::Direction::Column);
        stretch(column);
        auto &view = column.add<daw::ArrangementView>(store, hooks);
        view.showTiming(false);
        view.setMinSize({0.0f, 250.0f});
        view.setAlignSelf(ui::Align::Stretch);
        auto &editor = column.add<daw::AudioEditorView>(store, editorHooks);
        stretch(editor);
    });
}

// The piano roll on the demo project; with timedFrames, the settled view is timed (see renderPicture),
// and with `after`, one more frame is run after the picture
Picture renderPianoRoll(int timedFrames, Timing *timing, ui::FrameStats *after = nullptr)
{
    daw::ProjectStore store(daw::demoProject());
    selectFirstInstrumentClip(store);
    return renderPicture(1200, 520, daw::studioTheme(true), [&store](ui::Context &context) {
        stretch(context.root().add<daw::PianoRollView>(store, quietPianoRoll()));
    }, timedFrames, timing, after);
}

Picture scenePianoRoll()
{
    return renderPianoRoll(0, nullptr);
}

// The piano roll's tuning lane: the demo's first instrument clip with a note gliding up, one held
// down and one wavering, two of them selected, each drawn across its own span (and along its note)
Picture renderTuning(ui::FrameStats *after)
{
    daw::Project project = daw::demoProject();
    for (daw::Track &t : project.tracks) {
        if (t.kind != daw::TrackKind::Instrument || t.clips.empty() || t.clips[0].notes.size() < 4)
            continue;
        std::vector<daw::Note> &notes = t.clips[0].notes;
        notes[0].setCurve(daw::Expression::Tuning, {{0.0, 0.0f}, {notes[0].length * 0.5, 5.0f}});
        notes[1].setCurve(daw::Expression::Tuning, {{0.0, -3.0f}});
        std::vector<daw::ControlPoint> waver;
        for (int i = 0; i <= 16; ++i)
            waver.push_back({notes[2].length * i / 16.0, i % 2 ? 0.6f : -0.6f});
        notes[2].setCurve(daw::Expression::Tuning, waver);
        notes[0].selected = notes[2].selected = true;
        break;
    }
    daw::ProjectStore store(project);
    selectFirstInstrumentClip(store);
    return renderPicture(1200, 520, daw::studioTheme(true), [&store](ui::Context &context) {
        auto &roll = context.root().add<daw::PianoRollView>(store, quietPianoRoll());
        roll.setLane(daw::PianoRollView::kTuningLane);
        roll.setVelocityLane(true);
        stretch(roll);
    }, 0, nullptr, after);
}

Picture sceneTuning()
{
    return renderTuning(nullptr);
}

// The step sequencer on a drum clip; with timedFrames, the settled view is timed (see renderPicture),
// and with `after`, one more frame is run after the picture
Picture renderSteps(int timedFrames, Timing *timing, ui::FrameStats *after = nullptr)
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
    }, timedFrames, timing, after);
}

Picture sceneSteps()
{
    return renderSteps(0, nullptr);
}

// A long list in a scroll view: 2000 rows, of which only the ones on screen should draw.
// With timedFrames, the settled list is timed (see renderPicture)
Picture renderScroll(int timedFrames, Timing *timing)
{
    return renderPicture(600, 400, daw::studioTheme(true), [](ui::Context &context) {
        ui::ScrollView &scroll = context.root().add<ui::ScrollView>();
        stretch(scroll);
        for (int row = 0; row < 2000; ++row)
            scroll.content()
                .add<ui::ListItem>("Row " + std::to_string(row), [] {})
                .setSubtitle("Subtitle of row " + std::to_string(row));
    }, timedFrames, timing);
}

Picture sceneScroll()
{
    return renderScroll(0, nullptr);
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
// rect or clip paints again. Only without VYBN_VERIFY_CACHE, which paints every draw.
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

// With VYBN_VERIFY_CACHE every draw paints, so a matched key's paint is compared with the cached section
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

    // The draw cache's counts, or the verifier's check when VYBN_VERIFY_CACHE is set
    if (ui::DrawCache::verifying()) {
        std::printf("SKIP  %-44s %s\n", "Draw cache counts", "VYBN_VERIFY_CACHE is set, so every draw paints");
        verifierChecks(c, theme);
    } else {
        cacheChecks(c, theme);
        std::printf("SKIP  %-44s %s\n", "Draw cache verifier",
                    "VYBN_VERIFY_CACHE is not set; the verifier is checked by running the app");
    }
    std::printf("\n");
}

// A DAW view's scene, rendered again with one more frame after it has settled (see settledCacheChecks)
struct CacheScene {
    const char *name;
    Picture (*render)(ui::FrameStats *after);
};

const CacheScene kCacheScenes[] = {
    {"arrangement", [](ui::FrameStats *after) { return renderArrangement(0, nullptr, after); }},
    {"master", renderMaster},
    {"folding", renderFolding},
    {"takes", renderTakes},
    {"waveforms", [](ui::FrameStats *after) { return renderWaveforms(0, nullptr, after); }},
    {"pianoroll", [](ui::FrameStats *after) { return renderPianoRoll(0, nullptr, after); }},
    {"tuning", renderTuning},
    {"steps", [](ui::FrameStats *after) { return renderSteps(0, nullptr, after); }},
    {"synth", [](ui::FrameStats *after) { return renderSynth(0, nullptr, after); }},
    {"sidechain", renderSidechain},
};

// The cache scene of a scene, if its view has draw caches (null for the raw drawing scenes)
const CacheScene *cacheSceneOf(const std::string &name)
{
    for (const CacheScene &scene : kCacheScenes) {
        if (name == scene.name)
            return &scene;
    }
    return nullptr;
}

// A scene settled, then one more frame (`after`, from the same render as its picture): its views' draw
// caches serve every draw (hits, no misses) and the layout is skipped
void settledCacheChecks(Checker &c, const char *name, const ui::FrameStats &after)
{
    const std::string title = std::string(name) + ": a settled frame draws from its caches";
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
    {"automation", sceneAutomation},
    {"tempo", sceneTempo},
    {"tempo-hidden", sceneTempoHidden},
    {"takes", sceneTakes},
    {"master", sceneMaster},
    {"folding", sceneFolding},
    {"mixer", sceneMixer},
    {"radial", sceneRadial},
    {"waveforms", sceneWaveforms},
    {"warp", sceneWarp},
    {"pianoroll", scenePianoRoll},
    {"tuning", sceneTuning},
    {"steps", sceneSteps},
    {"scroll", sceneScroll},
    {"synth", sceneSynth},
    {"sidechain", sceneSidechain},
    {"pluginsidechain", scenePluginSidechain},
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
        std::printf("INFO render %s x%d: build %.2f ms, submit %.2f ms, %u prims, %zu KB, %.1f hits, %.1f misses, "
                    "layout skipped %.0f%%\n",
                    name, timing.frames, timing.buildMs, timing.submitMs, timing.prims, timing.uploadBytes / 1024,
                    timing.cacheHits, timing.cacheMisses, timing.layoutSkipped * 100.0);
        return 0;
    } catch (const std::exception &e) {
        std::printf("FAIL  %s timing failed: %s\n", name, e.what());
        return 1;
    }
}

// Renders one scene and checks it; returns its failures and keeps its frame stats
int checkScene(const std::string &name, const std::function<Picture()> &render, bool update, std::vector<std::pair<std::string, ui::FrameStats>> &rendered)
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

    // What each part took, for VYBN_TEST_TIMES (the runner's --times): each scene and each group of checks
    std::vector<std::pair<double, std::string>> times;
    auto timed = [&times](const std::string &name, const std::function<void()> &run) {
        const auto begin = std::chrono::steady_clock::now();
        run();
        times.emplace_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count(), name);
    };

    // A hidden window open for the whole run: GLFW starts when the first window opens and stops when
    // the last closes, and starting it enumerates the input devices (DirectInput on Windows), which can
    // take most of a second; without this every scene's window would pay that again
    engine::Window keeper(16, 16, "VYBN render test", false);

    // The primitive checks need no window and run whatever scene is asked for
    Checker prims;
    primChecks(prims);
    sectionChecks(prims);
    failures += prims.failures;

    // Each scene is rendered once: a DAW view's render also runs the one more frame its draw cache check
    // reads (after the picture is taken, so the picture is the same), instead of a second render
    bool found = only.empty();
    std::vector<std::pair<std::string, ui::FrameStats>> rendered;
    std::vector<std::pair<const char *, ui::FrameStats>> settled;
    std::vector<const char *> verifiedSkips; // cache scenes not checked because VYBN_VERIFY_CACHE makes every draw paint
    for (const SceneEntry &scene : kScenes) {
        if (!only.empty() && only != scene.name)
            continue;
        found = true;
        const CacheScene *cache = cacheSceneOf(scene.name);
        if (cache && !ui::DrawCache::verifying()) {
            ui::FrameStats after;
            timed(std::string("scene ") + scene.name, [&] { failures += checkScene(scene.name, [&] { return cache->render(&after); }, update, rendered); });
            settled.emplace_back(scene.name, after);
        } else {
            timed(std::string("scene ") + scene.name, [&] { failures += checkScene(scene.name, scene.render, update, rendered); });
            if (cache)
                verifiedSkips.push_back(scene.name);
        }
    }
    if (!found) {
        std::printf("FAIL  unknown scene \"%s\"\n", only.c_str());
        ++failures;
    }

    // The views' draw caches, on a frame after each scene has settled
    Checker caches;
    if (!settled.empty() || !verifiedSkips.empty())
        std::printf("Settled frames: their scenes draw from their draw caches\n\n");
    for (const auto &entry : settled)
        settledCacheChecks(caches, entry.first, entry.second);
    for (const char *name : verifiedSkips)
        std::printf("SKIP  %-44s %s\n", (std::string(name) + ": a settled frame draws from its caches").c_str(),
                    "VYBN_VERIFY_CACHE is set, so every draw paints");
    if (!settled.empty() || !verifiedSkips.empty())
        std::printf("\n");
    failures += caches.failures;

    // Editing automation in the arrangement (input handed to a laid-out view)
    if (only.empty()) {
        Checker editing;
        timed("automationEditChecks", [&] { automationEditChecks(editing); });
        timed("masterEditChecks", [&] { masterEditChecks(editing); });
        timed("tempoEditChecks", [&] { tempoEditChecks(editing); });
        timed("takeEditChecks", [&] { takeEditChecks(editing); });
        timed("markerEditChecks", [&] { markerEditChecks(editing); });
        timed("foldChecks", [&] { foldChecks(editing); });
        timed("foldUiChecks", [&] { foldUiChecks(editing); });
        timed("foldMixerChecks", [&] { foldMixerChecks(editing); });
        timed("foldDragChecks", [&] { foldDragChecks(editing); });
        timed("flexibleMixerChecks", [&] { flexibleMixerChecks(editing); });
        timed("tallStripChecks", [&] { tallStripChecks(editing); });
        timed("faderDragChecks", [&] { faderDragChecks(editing); });
        timed("browserDragChecks", [&] { browserDragChecks(editing); });
        timed("sidechainUiChecks", [&] { sidechainUiChecks(editing); });
        timed("sidechainDeviceViewChecks", [&] { sidechainDeviceViewChecks(editing); });
        if (bench())
            sidechainBench();
        timed("radialToggleChecks", [&] { radialToggleChecks(editing); });
        std::printf("\n");
        failures += editing.failures;
    }

    for (const auto &entry : rendered)
        printInfo(entry.first, entry.second);

    // The scenes timed on their settled frames: a benchmark (VYBN_TEST_BENCH), each a scene built again
    if (bench()) {
        failures += timeScene("synth", only, [](int frames, Timing *timing) { renderSynth(frames, timing); });
        failures += timeScene("waveforms", only, [](int frames, Timing *timing) { renderWaveforms(frames, timing); });
        failures += timeScene("scroll", only, [](int frames, Timing *timing) { renderScroll(frames, timing); });
        failures += timeScene("controls", only, [](int frames, Timing *timing) { renderControls(frames, timing); });
        failures += timeScene("arrangement", only, [](int frames, Timing *timing) { renderArrangement(frames, timing); });
        failures += timeScene("pianoroll", only, [](int frames, Timing *timing) { renderPianoRoll(frames, timing); });
        failures += timeScene("steps", only, [](int frames, Timing *timing) { renderSteps(frames, timing); });
    }
    if (flag("VYBN_TEST_TIMES")) {
        std::sort(times.begin(), times.end(), [](const auto &a, const auto &b) { return a.first > b.first; });
        for (size_t i = 0; i < times.size() && i < 10; ++i)
            std::printf("TIME  %-7.0f ms  %s\n", times[i].first, times[i].second.c_str());
    }
    if (failures == 0)
        std::printf("All passed: 0 failed (render)\n");
    else
        std::printf("%d FAILED (render)\n", failures);
    return failures == 0 ? 0 : 1;
}

}
