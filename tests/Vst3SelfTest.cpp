#include "Vst3SelfTest.h"
#include "Harness.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "plugin/Instance.h"

namespace tests {

namespace {

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

std::string text(const char *pattern, double value)
{
    char buffer[160];
    std::snprintf(buffer, sizeof buffer, pattern, value);
    return buffer;
}

constexpr double kRate = 48000.0;
constexpr uint32_t kFrames = 512;

// The gain's normalised value for a level in dB (it runs -60 to +12)
double gainAt(double db) { return (db + 60.0) / 72.0; }

struct Buffers {
    std::vector<float> in[2], out[2], key[2];
    float *inPtr[2];
    float *outPtr[2];
    float *keyPtr[2];

    Buffers()
    {
        for (int c = 0; c < 2; ++c) {
            in[c].assign(kFrames, 0.0f);
            out[c].assign(kFrames, 0.0f);
            key[c].assign(kFrames, 0.0f);
            inPtr[c] = in[c].data();
            outPtr[c] = out[c].data();
            keyPtr[c] = key[c].data();
        }
    }

    plugin::ProcessBlock block(bool input, const std::vector<plugin::Event> &events = {}, bool keyed = false)
    {
        plugin::ProcessBlock b;
        b.inputs = input ? inPtr : nullptr;
        b.inputChannels = input ? 2 : 0;
        b.outputs = outPtr;
        b.outputChannels = 2;
        b.keys = keyed ? keyPtr : nullptr;
        b.keyChannels = keyed ? 2 : 0;
        b.frames = kFrames;
        b.events = events.empty() ? nullptr : events.data();
        b.eventCount = static_cast<uint32_t>(events.size());
        return b;
    }

    float peak(int channel, uint32_t from = 0, uint32_t to = kFrames) const
    {
        float p = 0.0f;
        for (uint32_t i = from; i < to; ++i)
            p = std::max(p, std::fabs(out[channel][i]));
        return p;
    }
};

const plugin::ParamInfo *paramNamed(const std::vector<plugin::ParamInfo> &params, const std::string &name)
{
    for (const auto &p : params)
        if (p.name == name)
            return &p;
    return nullptr;
}

// The first visible top-level window of a process
HWND windowOf(DWORD process)
{
    std::pair<DWORD, HWND> found{process, nullptr};
    for (int i = 0; i < 100 && !found.second; ++i) {
        EnumWindows(
            [](HWND h, LPARAM l) -> BOOL {
                auto *f = reinterpret_cast<std::pair<DWORD, HWND> *>(l);
                DWORD owner = 0;
                GetWindowThreadProcessId(h, &owner);
                if (owner == f->first && IsWindowVisible(h)) {
                    f->second = h;
                    return FALSE;
                }
                return TRUE;
            },
            reinterpret_cast<LPARAM>(&found));
        if (!found.second)
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return found.second;
}

// Zero crossings on the left channel over some blocks of a sounding instrument
int crossings(plugin::Instance &sine, Buffers &s, int blocks)
{
    int n = 0;
    float last = 0.0f;
    for (int b = 0; b < blocks; ++b) {
        sine.process(s.block(false));
        for (uint32_t i = 0; i < kFrames; ++i) {
            if ((last < 0.0f) != (s.out[0][i] < 0.0f))
                ++n;
            last = s.out[0][i];
        }
    }
    return n;
}

// Plays blocks and polls until `done` says so, or a few seconds pass
template <class Done>
bool until(plugin::Instance &instance, Buffers &b, bool input, Done done)
{
    const auto end = Clock::now() + std::chrono::seconds(4);
    while (Clock::now() < end) {
        instance.process(b.block(input));
        instance.poll();
        if (done())
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
}

}

void runVst3Checks(Checker &c)
{
    const std::string bundle = VST3_TEST_PLUGIN;
    std::printf("\nVST3: %s\n\n", bundle.c_str());
    plugin::SandboxOptions sandbox;
    std::string error;

    // --- Scanning: the bundle, or its module on its own
    std::vector<plugin::Descriptor> found;
    const bool scanned = plugin::Instance::scan(bundle, sandbox, found, error);
    c.check(scanned && found.size() == 4, "a VST3 bundle's scan lists its four plugins (not its controller class)",
            scanned ? text("%.0f found", static_cast<double>(found.size())) : error);
    plugin::Descriptor gainD, sineD, duckD, twinD;
    for (const auto &d : found) {
        (d.name == "VST Gain" ? gainD : d.name == "VST Sine" ? sineD : d.name == "VST Duck" ? duckD : twinD) = d;
    }
    c.check(gainD.id.compare(0, 5, "vst3:") == 0 && gainD.vendor == "VYBN Tests" && gainD.version == "1.0.0" && gainD.effect() &&
                gainD.format() == "vst3" && gainD.path == bundle,
            "a VST3 plugin is described as any other: id, vendor, an effect, its file", gainD.id + " " + gainD.features);
    c.check(sineD.instrument() && sineD.hasFeature("synthesizer") && !duckD.instrument() && twinD.name == "Test Gain",
            "its subcategories become the same feature words", sineD.features);
    {
        std::vector<plugin::Descriptor> single;
        std::string why;
        const std::string module = (fs::u8path(bundle) / "Contents" / "x86_64-win" / "vybn-test-plugins.vst3").u8string();
        c.check(plugin::Instance::scan(module, sandbox, single, why) && single.size() == 4, "a single-file .vst3 scans too", why);
    }
    if (gainD.id.empty() || sineD.id.empty() || duckD.id.empty()) {
        c.check(false, "the VST3 test plugins can be scanned");
        return;
    }

    // --- The gain: audio, parameters, latency, its editor, state
    auto gain = plugin::Instance::create(gainD, sandbox, error);
    c.check(gain && gain->info().inputChannels == 2 && gain->info().outputChannels == 2 && gain->info().params && gain->info().state &&
                gain->info().gui && gain->info().embedded,
            "VST Gain loads in the sandbox, with stereo buses, parameters, state and an editor", gain ? "" : error);
    if (!gain || !gain->activate(kRate, error)) {
        c.check(false, "VST Gain activates", error);
        return;
    }
    std::vector<plugin::ParamInfo> params;
    gain->params(params, error);
    const plugin::ParamInfo *level = paramNamed(params, "Gain");
    const plugin::ParamInfo *mute = paramNamed(params, "Mute");
    const plugin::ParamInfo *peak = paramNamed(params, "Peak");
    const plugin::ParamInfo *latency = paramNamed(params, "Latency");
    c.check(params.size() == 4 && level && level->max == 1.0 && std::fabs(level->value - gainAt(0.0)) < 1e-6 && mute && mute->stepped() &&
                mute->max == 1.0 && peak && peak->readOnly() && latency && latency->stepped() && latency->max == 8.0,
            "its parameters: continuous 0-1, stepped by their steps, read-only kept", text("%.0f parameters", static_cast<double>(params.size())));

    Buffers b;
    for (uint32_t i = 0; i < kFrames; ++i)
        b.in[0][i] = b.in[1][i] = 0.5f * static_cast<float>(std::sin(2.0 * 3.14159265 * 440.0 * i / kRate));
    bool ok = gain->process(b.block(true));
    float worst = 0.0f;
    for (int ch = 0; ch < 2; ++ch)
        for (uint32_t i = 0; i < kFrames; ++i)
            worst = std::max(worst, std::fabs(b.out[ch][i] - b.in[ch][i]));
    c.check(ok && worst < 1e-5f, "at 0 dB its audio comes back unchanged", text("max difference %.2g", worst));

    gain->setParam(0, gainAt(-6.0206));
    ok = gain->process(b.block(true));
    c.check(ok && std::fabs(b.peak(0) / 0.5f - 0.5f) < 0.01f, "a value set by the app reaches its processor", text("level x%.3f", b.peak(0) / 0.5f));
    gain->poll();
    bool sawPeak = false;
    for (const auto &ch : gain->takeChanges())
        sawPeak = sawPeak || (ch.id == 2 && std::fabs(ch.value - 0.25) < 0.01);
    c.check(sawPeak, "its processor's output parameter changes come back");
    std::string shown, steps;
    const bool texts = gain->paramText(0, gainAt(-6.0206), shown) && gain->paramText(3, 2.0, steps);
    c.check(texts && shown == "-6.0 dB" && steps == "1024 frames", "values are shown in its words (a stepped one by its step)", shown + ", " + steps);
    {
        // The controller hears of the app's value after the block, so its interface shows it
        std::vector<plugin::ParamInfo> after;
        const bool heard = until(*gain, b, true, [&] {
            const plugin::ParamInfo *g = gain->params(after, error) ? paramNamed(after, "Gain") : nullptr;
            return g && std::fabs(g->value - gainAt(-6.0206)) < 1e-6;
        });
        c.check(heard, "its edit controller is given the values its processor got");
    }

    // Latency: it asks to be restarted, as VST3 plugins do
    gain->setParam(3, 2.0);
    const bool restarted = until(*gain, b, true, [&] { return gain->latency() == 1024; });
    c.check(restarted, "a latency change restarts it and comes across", text("%.0f frames", gain->latency()));
    gain->setParam(3, 0.0);
    until(*gain, b, true, [&] { return gain->latency() == 0; });

    // Its editor in a window of the host's own: a click on its right half steps Gain up, through the host
    {
        std::string why;
        const bool opened = gain->openGui(why);
        HWND window = opened ? windowOf(gain->hostProcessId()) : nullptr;
        HWND editor = window ? FindWindowExW(window, nullptr, L"VybnTestVst3Editor", nullptr) : nullptr;
        c.check(opened && window && editor && gain->guiOpen(), "its editor opens in a window of its own", why);
        if (editor) {
            gain->poll();
            gain->takeChanges();
            PostMessageW(editor, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(250, 20));
            bool began = false, value = false, ended = false;
            until(*gain, b, true, [&] {
                for (const auto &ch : gain->takeChanges()) {
                    began = began || (ch.kind == plugin::ChangeKind::GestureBegin && ch.id == 0);
                    value = value || (ch.kind == plugin::ChangeKind::Value && ch.id == 0 && std::fabs(ch.value - gainAt(-5.0206)) < 1e-4);
                    ended = ended || (ch.kind == plugin::ChangeKind::GestureEnd && ch.id == 0);
                }
                return began && value && ended;
            });
            c.check(began && value && ended, "an edit in its editor comes back as a gesture");
            gain->process(b.block(true));
            c.check(std::fabs(b.peak(0) / 0.5f - static_cast<float>(std::pow(10.0, -5.0206 / 20.0))) < 0.01f,
                    "and reaches its processor", text("level x%.3f", b.peak(0) / 0.5f));
            PostMessageW(window, WM_CLOSE, 0, 0);
            for (int i = 0; i < 100 && gain->guiOpen(); ++i) {
                gain->poll();
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            c.check(!gain->guiOpen(), "closing its window is noticed");
        }
    }

    // Embedded: the same frame, capture and replayed input as a CLAP plugin's
    {
        std::string why;
        uint32_t w = 0, h = 0;
        const bool embedded = gain->openEmbeddedGui(w, h, why);
        c.check(embedded && w == 300 && h == 120, "its editor opens embedded, at its size", embedded ? text("%.0f x ", w) + text("%.0f", h) : why);
        std::vector<uint8_t> pixels;
        uint32_t fw = 0, fh = 0, sequence = 0;
        bool grey = false;
        const auto end = Clock::now() + std::chrono::seconds(3);
        while (embedded && Clock::now() < end && !grey) {
            if (gain->readGuiFrame(pixels, fw, fh, sequence) && pixels.size() >= 4) {
                const int sum = pixels[pixels.size() / 2] + pixels[pixels.size() / 2 + 1] + pixels[pixels.size() / 2 + 2];
                grey = fw == 300 && fh == 120 && sum > 300 && sum < 500;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        c.check(grey, "its pictures arrive (its grey)");
        if (embedded) {
            gain->poll();
            gain->takeChanges();
            plugin::GuiInput press;
            press.kind = plugin::GuiInputKind::Down;
            press.x = 50; // the left half: a step down
            press.y = 20;
            gain->sendGuiInput(press);
            press.kind = plugin::GuiInputKind::Up;
            gain->sendGuiInput(press);
            bool value = false;
            until(*gain, b, true, [&] {
                for (const auto &ch : gain->takeChanges())
                    value = value || (ch.kind == plugin::ChangeKind::Value && ch.id == 0 && std::fabs(ch.value - gainAt(-6.0206)) < 1e-4);
                return value;
            });
            c.check(value, "a click sent from the app edits it");
            gain->closeGui();
            c.check(!gain->guiEmbedded(), "its embedded editor closes");
        }
    }

    // State: both halves in one blob, read by a second instance
    std::string state;
    c.check(gain->saveState(state, error) && state.compare(0, 4, "VYB3") == 0, "its state saves", error);
    {
        auto other = plugin::Instance::create(gainD, sandbox, error);
        std::vector<plugin::ParamInfo> after;
        const bool loaded = other && other->loadState(state, error) && other->params(after, error);
        const plugin::ParamInfo *g = loaded ? paramNamed(after, "Gain") : nullptr;
        c.check(g && std::fabs(g->value - gainAt(-6.0206)) < 1e-5, "a second instance loads it", g ? text("Gain %.4f", g->value) : error);
        c.check(other && !other->loadState("nonsense", error), "a state that isn't its is refused", error);
    }
    gain.reset();

    // --- The sine: notes, the bend through its MIDI mapping
    auto sine = plugin::Instance::create(sineD, sandbox, error);
    c.check(sine && sine->info().notePorts == 1 && sine->info().inputChannels == 0, "VST Sine loads, with an event input and no audio input",
            sine ? "" : error);
    if (sine && sine->activate(kRate, error)) {
        Buffers s;
        std::vector<plugin::Event> on(1);
        on[0].kind = plugin::EventKind::NoteOn;
        on[0].frame = 128;
        on[0].id = 69;
        on[0].value = 1.0;
        sine->process(s.block(false, on));
        c.check(s.peak(0, 0, 128) == 0.0f && s.peak(0, 128, kFrames) > 0.3f, "a note starts on its frame",
                text("peak %.2f", s.peak(0, 128, kFrames)));
        const int plain = crossings(*sine, s, 8);
        std::vector<plugin::Event> bend(1);
        bend[0].kind = plugin::EventKind::Controller;
        bend[0].id = 128;
        bend[0].value = 1.0;
        sine->process(s.block(false, bend));
        const int bent = crossings(*sine, s, 8);
        const double ratio = static_cast<double>(bent) / plain;
        c.check(std::fabs(ratio - std::pow(2.0, 2.0 / 12.0)) < 0.03, "the bend reaches it through its MIDI mapping (two semitones up)",
                text("x%.3f", ratio));
        std::vector<plugin::ParamInfo> after;
        const bool shownBend = until(*sine, s, false, [&] {
            const plugin::ParamInfo *p = sine->params(after, error) ? paramNamed(after, "Bend") : nullptr;
            return p && p->value == 1.0;
        });
        c.check(shownBend, "and its controller shows it");
        std::vector<plugin::Event> off(1);
        off[0].kind = plugin::EventKind::AllNotesOff;
        sine->process(s.block(false, off));
        sine->process(s.block(false));
        c.check(s.peak(0) == 0.0f, "all notes off silences it");
    }
    sine.reset();

    // --- The duck: an aux input as its sidechain, its controller a class of its own
    {
        uint32_t channels = 0;
        std::string why;
        c.check(plugin::Instance::probeSidechain(bundle, duckD.id, sandbox, channels, why) && channels == 2,
                "a probe finds its aux input, the sidechain", why);
        error.clear();
        auto duck = plugin::Instance::create(duckD, sandbox, error);
        std::vector<plugin::ParamInfo> list;
        const bool made = duck && duck->activate(kRate, error) && duck->params(list, error);
        const plugin::ParamInfo *hidden = made ? paramNamed(list, "Hidden") : nullptr;
        c.check(made && list.size() == 3 && hidden && hidden->hidden(), "VST Duck loads, its separate controller connected", error);
        if (made) {
            Buffers d;
            for (uint32_t i = 0; i < kFrames; ++i)
                d.in[0][i] = d.in[1][i] = 0.5f;
            duck->process(d.block(true));
            const float unkeyed = d.out[0][10];
            for (uint32_t i = 0; i < kFrames; ++i)
                d.key[0][i] = d.key[1][i] = 0.75f;
            duck->process(d.block(true, {}, true));
            const float keyed = d.out[0][10];
            c.check(std::fabs(unkeyed - 0.5f) < 1e-6f && std::fabs(keyed - 0.125f) < 1e-5f, "the block's key reaches its sidechain bus",
                    text("%.3f unkeyed, ", unkeyed) + text("%.3f keyed", keyed));
        }
    }
}

}
