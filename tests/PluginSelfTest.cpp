#include "PluginSelfTest.h"
#include "Harness.h"
#include "Vst3SelfTest.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "env/Environment.h"
#include "env/Scheduler.h"
#include "plugin/Instance.h"
#include "audio/AudioSystem.h"
#include "audio/Effects.h"
#include "audio/Midi.h"
#include "audio/Synth.h"
#include "daw/audio/AudioBridge.h"
#include "daw/audio/AudioPool.h"
#include "daw/library/Library.h"
#include "daw/model/Project.h"
#include "daw/model/ProjectStore.h"
#include "daw/model/Routing.h"
#include "daw/app/Settings.h"
#include "audio/DeviceChain.h"
#include "audio/PluginEffect.h"
#include "audio/NotePlayer.h"
#include "audio/PluginInstrument.h"
#include "audio/Track.h"
#include "plugin/Manager.h"
#include "plugin/Slot.h"

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

double since(Clock::time_point start) { return std::chrono::duration<double>(Clock::now() - start).count(); }

constexpr double kRate = 48000.0;
constexpr uint32_t kFrames = 512;

// A plugin as a device
audio::DeviceSetting pluginDevice(const std::string &id, const std::string &name)
{
    audio::DeviceSetting d;
    d.kind = audio::DeviceKind::Plugin;
    d.id = id;
    d.name = name;
    return d;
}

// Stereo buffers for one block, and a block pointing at them
struct Buffers {
    std::vector<float> in[2], out[2];
    float *inPtr[2];
    float *outPtr[2];

    Buffers()
    {
        for (int c = 0; c < 2; ++c) {
            in[c].assign(kFrames, 0.0f);
            out[c].assign(kFrames, 0.0f);
            inPtr[c] = in[c].data();
            outPtr[c] = out[c].data();
        }
    }

    plugin::ProcessBlock block(bool input, const std::vector<plugin::Event> &events = {})
    {
        plugin::ProcessBlock b;
        b.inputs = input ? inPtr : nullptr;
        b.inputChannels = input ? 2 : 0;
        b.outputs = outPtr;
        b.outputChannels = 2;
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

}

int runPluginSelfTest()
{
    Checker c;
    const std::string file = ENGINE_TEST_PLUGIN;
    std::printf("Plugin self-test: %s\n\n", file.c_str());
    {
        std::error_code ec;
        c.check(fs::exists(plugin::hostExecutable(), ec), "the plugin host is built", plugin::hostExecutable());
        c.check(fs::exists(fs::u8path(file), ec), "the test plugins are built");
    }
    plugin::SandboxOptions sandbox;

    // --- Scanning
    std::vector<plugin::Descriptor> found;
    std::string error;
    auto start = Clock::now();
    const bool scanned = plugin::Instance::scan(file, sandbox, found, error);
    c.check(scanned && found.size() == 5, "a scan lists the file's five plugins", scanned ? text("in %.0f ms", since(start) * 1000) : error);
    plugin::Descriptor gainD, sineD, faultyD, duckD;
    for (const auto &d : found) {
        if (d.id == "vybn.test.gain")
            gainD = d;
        else if (d.id == "vybn.test.sine")
            sineD = d;
        else if (d.id == "vybn.test.faulty")
            faultyD = d;
        else if (d.id == "vybn.test.duck")
            duckD = d;
    }
    c.check(gainD.name == "Test Gain" && gainD.vendor == "VYBN" && gainD.effect() && !gainD.instrument(), "the gain is an effect, named");
    c.check(sineD.instrument() && sineD.hasFeature("synthesizer"), "the sine is an instrument", sineD.features);
    c.check(gainD.path == file, "descriptors know their file");
    {
        std::vector<plugin::Descriptor> none;
        std::string why;
        const bool ok = plugin::Instance::scan(file + ".missing", sandbox, none, why);
        c.check(!ok && !why.empty(), "a missing file is refused with a reason", why);
    }
    if (gainD.id.empty() || sineD.id.empty() || faultyD.id.empty() || duckD.id.empty()) {
        std::printf("\nFAILED: the test plugins couldn't be scanned, so nothing else can be checked\n");
        return 1;
    }

    // --- The gain: audio and parameters
    start = Clock::now();
    std::unique_ptr<plugin::Instance> gain = plugin::Instance::create(gainD, sandbox, error);
    c.check(gain != nullptr, "the gain loads in a host of its own", gain ? text("in %.0f ms", since(start) * 1000) : error);
    if (!gain)
        return 1;
    if (gain->contained())
        c.check(true, "it runs inside the AppContainer");
    else
        c.check(false, "it runs inside the AppContainer", gain->sandboxNote());
    c.check(gain->info().inputChannels == 2 && gain->info().outputChannels == 2 && gain->info().params && gain->info().state,
            "it has stereo ports, parameters and state");
    c.check(gain->activate(kRate, error), "it activates", error);

    std::vector<plugin::ParamInfo> params;
    c.check(gain->params(params, error) && params.size() == 4, "it lists its four parameters", error);
    const plugin::ParamInfo *gainParam = paramNamed(params, "Gain");
    c.check(gainParam && gainParam->min == -60.0 && gainParam->max == 12.0 && gainParam->value == 0.0, "Gain's range and value come across");
    const plugin::ParamInfo *peakParam = paramNamed(params, "Peak");
    c.check(peakParam && peakParam->readOnly() && paramNamed(params, "Mute")->stepped(), "flags come across (read-only, stepped)");

    Buffers b;
    for (uint32_t i = 0; i < kFrames; ++i)
        b.in[0][i] = b.in[1][i] = 0.5f * static_cast<float>(std::sin(2.0 * 3.14159265 * 440.0 * i / kRate));
    bool ok = gain->process(b.block(true));
    float worst = 0.0f;
    for (int ch = 0; ch < 2; ++ch)
        for (uint32_t i = 0; i < kFrames; ++i)
            worst = std::max(worst, std::fabs(b.out[ch][i] - b.in[ch][i]));
    c.check(ok && worst < 1e-6f, "at 0 dB the audio comes back unchanged", text("max difference %.2g", worst));

    gain->setParam(1, -6.0206);
    ok = gain->process(b.block(true));
    const float ratio = b.peak(0) / 0.5f;
    c.check(ok && std::fabs(ratio - 0.5f) < 0.01f, "a parameter set by the app reaches the next block", text("level x%.3f", ratio));

    gain->poll();
    const auto changes = gain->takeChanges();
    bool sawPeak = false;
    for (const auto &ch : changes)
        if (ch.id == 3 && std::fabs(ch.value - 0.25) < 0.01)
            sawPeak = true;
    c.check(sawPeak, "the plugin's own parameter changes come back", text("%.0f changes", static_cast<double>(changes.size())));

    std::string shown;
    c.check(gain->paramText(1, -6.0206, shown) && shown == "-6.0 dB", "values are shown in the plugin's words", shown);

    // Timing of the round trip
    {
        const int blocks = 400;
        double worstMs = 0.0;
        start = Clock::now();
        for (int i = 0; i < blocks; ++i) {
            const auto t = Clock::now();
            gain->process(b.block(true));
            worstMs = std::max(worstMs, since(t) * 1000.0);
        }
        const double each = since(start) / blocks * 1e6;
        c.check(each < 2000.0 && gain->missed() == 0, "a block's round trip through the sandbox is quick",
                text("%.0f us each, ", each) + text("worst %.2f ms", worstMs));
    }

    // Its own window, from inside the sandbox: opened, clicked (a change coming back as a gesture)
    // and closed by the user
    {
        std::string why;
        const bool opened = gain->info().gui && gain->openGui(why);
        std::pair<DWORD, HWND> found{gain->hostProcessId(), nullptr};
        for (int i = 0; i < 100 && opened && !found.second; ++i) {
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
        HWND window = found.second;
        c.check(opened && window && gain->guiOpen(), "its own window opens from the sandbox", why);
        if (window) {
            gain->poll();
            gain->takeChanges();
            PostMessageW(window, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(400, 20)); // the right half: +1 dB
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            gain->process(b.block(true));
            gain->poll();
            bool began = false, value = false, ended = false;
            for (const auto &ch : gain->takeChanges()) {
                began = began || (ch.kind == plugin::ChangeKind::GestureBegin && ch.id == 1);
                value = value || (ch.kind == plugin::ChangeKind::Value && ch.id == 1 && std::fabs(ch.value + 5.0206) < 1e-3);
                ended = ended || (ch.kind == plugin::ChangeKind::GestureEnd && ch.id == 1);
            }
            c.check(began && value && ended, "a change in its window comes back as a gesture");
            PostMessageW(window, WM_CLOSE, 0, 0);
            for (int i = 0; i < 100 && gain->guiOpen(); ++i) {
                gain->poll();
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            c.check(!gain->guiOpen(), "closing its window is noticed");
            gain->setParam(1, -6.0206); // back where the state checks expect it
            gain->process(b.block(true));
        }
    }

    // Its interface embedded in the app: captured inside the sandbox into shared memory (the
    // pictures the app draws), with the app's mouse sent back to it
    {
        std::string why;
        uint32_t w = 0, h = 0;
        const bool embedded = gain->info().embedded && gain->openEmbeddedGui(w, h, why);
        c.check(embedded && w == 420 && h == 140 && gain->guiEmbedded(), "its interface opens embedded, at its size",
                embedded ? text("%.0f x ", w) + text("%.0f", h) : why);
        std::vector<uint8_t> pixels;
        uint32_t fw = 0, fh = 0, sequence = 0;
        // The first pictures can come before it has painted: wait for one that is drawn
        bool got = false;
        size_t light = 0, dark = 0;
        const auto until = Clock::now() + std::chrono::seconds(3);
        while (embedded && Clock::now() < until && !(got && light > dark)) {
            if (gain->readGuiFrame(pixels, fw, fh, sequence)) {
                got = true;
                light = dark = 0;
                for (size_t i = 0; i + 3 < pixels.size(); i += 4) {
                    const int sum = pixels[i] + pixels[i + 1] + pixels[i + 2];
                    light += sum > 600 ? 1 : 0;
                    dark += sum < 200 ? 1 : 0;
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        c.check(got && fw == 420 && fh == 140 && light > dark && dark > 50, "its pictures arrive, drawn (its text on its background)",
                got ? text("%.0f dark pixels", static_cast<double>(dark)) : "no picture");
        uint32_t again = sequence;
        std::this_thread::sleep_for(std::chrono::milliseconds(80));
        // (a loaded machine may take a while to paint again: wait for it)
        bool more = gain->readGuiFrame(pixels, fw, fh, again) && again != sequence;
        for (int i = 0; i < 100 && !more; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            more = gain->readGuiFrame(pixels, fw, fh, again) && again != sequence;
        }
        c.check(more, "and keep coming");
        if (embedded) {
            gain->poll();
            gain->takeChanges();
            plugin::GuiInput press;
            press.kind = plugin::GuiInputKind::Down;
            press.x = 400; // the right half: +1 dB
            press.y = 20;
            gain->sendGuiInput(press);
            press.kind = plugin::GuiInputKind::Up;
            gain->sendGuiInput(press);
            bool value = false, began = false;
            for (int i = 0; i < 50 && !value; ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                gain->process(b.block(true));
                gain->poll();
                for (const auto &ch : gain->takeChanges()) {
                    began = began || (ch.kind == plugin::ChangeKind::GestureBegin && ch.id == 1);
                    value = value || (ch.kind == plugin::ChangeKind::Value && ch.id == 1 && std::fabs(ch.value + 5.0206) < 1e-3);
                }
            }
            c.check(began && value, "a click sent from the app changes it, as a gesture");
            gain->closeGui();
            c.check(!gain->guiEmbedded() && !gain->guiOpen(), "its embedded interface closes");
            gain->setParam(1, -6.0206);
            gain->process(b.block(true));
        }
    }

    // State
    std::string state;
    c.check(gain->saveState(state, error) && state.find("gain=-6.02") == 0, "its state saves", state);
    {
        auto other = plugin::Instance::create(gainD, sandbox, error);
        std::vector<plugin::ParamInfo> after;
        const bool loaded = other && other->loadState(state, error) && other->params(after, error);
        const plugin::ParamInfo *g = loaded ? paramNamed(after, "Gain") : nullptr;
        c.check(g && std::fabs(g->value + 6.0206) < 1e-4, "a second instance loads it", g ? text("Gain %.4f dB", g->value) : error);
        c.check(other && !other->loadState("nonsense", error), "a state it can't read is refused", error);
    }

    // --- The sine: notes on the right frame
    error.clear();
    auto sine = plugin::Instance::create(sineD, sandbox, error);
    c.check(sine && sine->info().notePorts == 1 && sine->info().inputChannels == 0, "the sine loads, with a note input and no audio input",
            sine ? "" : error);
    if (sine && sine->activate(kRate, error)) {
        Buffers s;
        std::vector<plugin::Event> events(1);
        events[0].kind = plugin::EventKind::NoteOn;
        events[0].frame = 128;
        events[0].id = 69;
        events[0].value = 1.0;
        sine->process(s.block(false, events));
        c.check(s.peak(0, 0, 128) == 0.0f && s.peak(0, 128, kFrames) > 0.4f, "a note starts on its frame",
                text("peak after %.2f", s.peak(0, 128, kFrames)));
        // Pitch from zero crossings over the next blocks
        int crossings = 0;
        float last = s.out[0][kFrames - 1];
        for (int blockIndex = 0; blockIndex < 10; ++blockIndex) {
            sine->process(s.block(false));
            for (uint32_t i = 0; i < kFrames; ++i) {
                if ((last < 0.0f) != (s.out[0][i] < 0.0f))
                    ++crossings;
                last = s.out[0][i];
            }
        }
        const double hz = crossings / 2.0 / (10.0 * kFrames / kRate);
        c.check(std::fabs(hz - 440.0) < 10.0, "A4 sounds at 440 Hz", text("%.1f Hz", hz));
        // A note's tuning, against its key: another key's leaves it, its own takes it up an octave
        auto pitch = [&] {
            int n = 0;
            float before = s.out[0][kFrames - 1];
            for (int blockIndex = 0; blockIndex < 10; ++blockIndex) {
                sine->process(s.block(false));
                for (uint32_t i = 0; i < kFrames; ++i) {
                    if ((before < 0.0f) != (s.out[0][i] < 0.0f))
                        ++n;
                    before = s.out[0][i];
                }
            }
            return n / 2.0 / (10.0 * kFrames / kRate);
        };
        std::vector<plugin::Event> tune(1);
        tune[0].kind = plugin::EventKind::NoteExpression;
        tune[0].id = 70;
        tune[0].expression = static_cast<uint32_t>(plugin::NoteExpression::Tuning);
        tune[0].value = 12.0;
        sine->process(s.block(false, tune));
        const double other = pitch();
        tune[0].id = 69;
        sine->process(s.block(false, tune));
        const double octave = pitch();
        c.check(std::fabs(other - 440.0) < 10.0 && std::fabs(octave - 880.0) < 15.0,
                "a note's tuning reaches its voice alone, by its note id", text("%.1f Hz, then %.1f Hz", other, octave));
        events[0].kind = plugin::EventKind::NoteOff;
        events[0].frame = 0;
        sine->process(s.block(false, events));
        sine->process(s.block(false));
        c.check(s.peak(0) == 0.0f, "a note off lets it fade out");
    }

    // --- The sandbox
    error.clear();
    auto faulty = plugin::Instance::create(faultyD, sandbox, error);
    c.check(faulty != nullptr, "the faulty plugin loads", error);
    if (!faulty)
        return 1;
    // What the faulty plugin's probes said as it was made: writing in the profile, and beside its own file
    auto probes = [&](plugin::Instance &instance) {
        std::string profile, own;
        const auto until = Clock::now() + std::chrono::seconds(2);
        while ((profile.empty() || own.empty()) && Clock::now() < until) {
            instance.poll();
            for (const std::string &line : instance.takeLog()) {
                if (line.find("probe:") != std::string::npos)
                    profile = line;
                if (line.find("probe-own-folder:") != std::string::npos)
                    own = line;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return std::make_pair(profile, own);
    };
    {
        const auto said = probes(*faulty);
        if (faulty->contained()) {
            c.check(said.first.find("write denied") != std::string::npos, "the sandbox stops a plugin writing to the profile", said.first);
            c.check(said.second.find("write denied") != std::string::npos, "or beside its own file, unless allowed", said.second);
        } else {
            c.skip("the sandbox stops a plugin writing to the profile", faulty->sandboxNote());
        }
    }
    // A folder granted for writing (its presets, its database) can be written in
    {
        std::error_code ec;
        const fs::path folder = fs::temp_directory_path() / "vybn-plugin-write-test";
        fs::remove_all(folder, ec);
        fs::create_directories(folder, ec);
        fs::copy_file(fs::u8path(file), folder / "copy.clap", ec);
        plugin::Descriptor there = faultyD;
        there.path = (folder / "copy.clap").u8string();
        plugin::SandboxOptions allowed = sandbox;
        allowed.writable = {folder.u8string()};
        auto writer = plugin::Instance::create(there, allowed, error);
        const auto said = writer ? probes(*writer) : std::make_pair(std::string(), std::string());
        if (writer && writer->contained())
            c.check(said.second.find("write allowed") != std::string::npos && said.first.find("write denied") != std::string::npos,
                    "a folder allowed for writing can be written in (and only it)", said.second);
        writer.reset();
        fs::remove_all(folder, ec);
    }

    // A crash
    faulty->activate(kRate, error);
    Buffers f;
    for (uint32_t i = 0; i < kFrames; ++i)
        f.in[0][i] = f.in[1][i] = 0.25f;
    c.check(faulty->process(f.block(true)) && f.peak(0) > 0.2f, "before its fault it passes audio");
    faulty->setParam(1, 1.0); // crash
    start = Clock::now();
    ok = faulty->process(f.block(true));
    const double crashMs = since(start) * 1000.0;
    c.check(!ok && f.peak(0) == 0.0f && crashMs < 0.85 * kFrames / kRate * 1000.0 + 3.0, "a crashing block plays silence, in time",
            text("returned in %.1f ms", crashMs));
    for (int i = 0; i < 200 && faulty->status() == plugin::Status::Ready; ++i) {
        faulty->poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    c.check(faulty->status() == plugin::Status::Crashed, "the watchdog reports the crash", faulty->statusText());
    start = Clock::now();
    ok = faulty->process(f.block(true));
    c.check(!ok && since(start) < 0.001, "a crashed plugin costs the audio thread nothing", text("%.0f us", since(start) * 1e6));
    c.check(!faulty->params(params, error), "control calls to it fail with a reason", error);

    // A hang
    faulty = plugin::Instance::create(faultyD, sandbox, error);
    if (faulty && faulty->activate(kRate, error)) {
        faulty->hangSeconds = 0.3;
        faulty->setParam(1, 2.0);
        start = Clock::now();
        ok = faulty->process(f.block(true));
        const double waited = since(start) * 1000.0;
        const double budget = 0.85 * kFrames / kRate * 1000.0;
        c.check(!ok && waited < budget + 3.0, "a hanging block gives up at its deadline", text("after %.1f ms", waited));
        start = Clock::now();
        ok = faulty->process(f.block(true));
        c.check(!ok && since(start) < 0.001, "later blocks don't wait for it", text("%.0f us", since(start) * 1e6));
        for (int i = 0; i < 200 && faulty->status() == plugin::Status::Ready; ++i) {
            faulty->poll();
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        c.check(faulty->status() == plugin::Status::Hung, "the watchdog stops it", faulty->statusText());
    } else {
        c.check(false, "a hanging plugin loads", error);
    }

    // Too slow, then better
    faulty = plugin::Instance::create(faultyD, sandbox, error);
    if (faulty && faulty->activate(kRate, error)) {
        faulty->setParam(1, 3.0); // 60 ms a block
        int answered = 0;
        for (int i = 0; i < 20; ++i) {
            answered += faulty->process(f.block(true)) ? 1 : 0;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            faulty->poll();
        }
        const uint32_t missed = faulty->missed();
        c.check(missed > 10 && faulty->status() == plugin::Status::Ready, "a slow plugin misses blocks but isn't stopped",
                text("%.0f missed", missed) + text(", %.0f answered", answered));
        faulty->setParam(1, 0.0);
        bool recovered = false;
        for (int i = 0; i < 100 && !recovered; ++i) {
            recovered = faulty->process(f.block(true)) && f.peak(0) > 0.2f;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        c.check(recovered, "once it is quick again it is heard again");
    }

    // --- VST3 plugins, through the same sandbox
    runVst3Checks(c);

    // --- The manager (the test plugins' folder holds both formats)
    {
        env::Environment environment;
        plugin::PluginConfig config;
        config.folders = {fs::u8path(file).parent_path().u8string()};
        plugin::PluginManager manager(environment, config);
        const auto scanStart = Clock::now();
        manager.rescan();
        const auto until = Clock::now() + std::chrono::seconds(20);
        while (manager.scanning() && Clock::now() < until)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        std::printf("INFO plugin scan: the manager scans the file in %.0f ms, with %zu effect plugins probed for a sidechain port, each in a host of its own\n",
                    since(scanStart) * 1000, manager.lastScan().probed);
        const auto list = manager.plugins();
        c.check(!manager.scanning() && list.size() == 8 && manager.version() == 1, "the manager scans a folder, CLAP and VST3",
                text("%.0f plugins", static_cast<double>(list.size())));
        {
            // The VST3 twin of Test Gain (its name and vendor) isn't listed beside it, but is still found by its id
            size_t gains = 0;
            std::string twinId;
            for (const auto &d : list)
                gains += d.name == "Test Gain" ? 1 : 0;
            std::vector<plugin::Descriptor> vst3;
            std::string why;
            plugin::Instance::scan(VST3_TEST_PLUGIN, sandbox, vst3, why);
            for (const auto &d : vst3)
                if (d.name == "Test Gain")
                    twinId = d.id;
            plugin::Descriptor twin;
            c.check(gains == 1 && !twinId.empty() && manager.find(twinId, twin) && twin.format() == "vst3",
                    "a plugin in both formats is listed once, as CLAP; the other is still found by its id");
            // On a track it is a plugin like any other: its slot loads it and the project's values reach it
            if (!twinId.empty()) {
                audio::DeviceChain chain;
                chain.setPlugins(&manager);
                chain.prepare(kRate, kFrames);
                std::vector<audio::DeviceSetting> settings = {pluginDevice(twinId, "Test Gain")};
                settings[0].setValue(0, (60.0 - 6.0206) / 72.0);
                chain.apply(settings);
                auto *effect = dynamic_cast<audio::PluginEffect *>(chain.effect(0));
                const auto until = Clock::now() + std::chrono::seconds(10);
                while (effect && effect->slot()->state() == plugin::Slot::State::Loading && Clock::now() < until) {
                    chain.collect();
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                }
                Buffers e;
                for (uint32_t i = 0; i < kFrames; ++i)
                    e.out[0][i] = e.out[1][i] = 0.5f;
                chain.process(e.outPtr[0], e.outPtr[1], kFrames);
                c.check(effect && effect->slot()->state() == plugin::Slot::State::Ready && std::fabs(e.out[0][10] - 0.25f) < 0.005f,
                        "a VST3 effect plays in a device chain, with the project's values", text("%.3f", e.out[0][10]));
            }
        }
        error.clear();
        auto made = manager.create("vybn.test.gain", error);
        c.check(made && made->status() == plugin::Status::Ready, "and makes an instance by id", error);
        c.check(!manager.create("vybn.test.nothing", error) && !error.empty(), "an unknown id is refused", error);
        // A plugin effect in a chain: the sound passes while it loads, then the plugin plays it,
        // values from the project reach it, dropping a value brings the state back, and a crash
        // silences it until reloaded
        {
            audio::DeviceChain chain;
            chain.setPlugins(&manager);
            chain.prepare(kRate, kFrames);
            std::vector<audio::DeviceSetting> settings = {pluginDevice("vybn.test.gain", "Test Gain")};
            chain.apply(settings);
            Buffers e;
            auto fill = [&e] {
                for (uint32_t i = 0; i < kFrames; ++i)
                    e.out[0][i] = e.out[1][i] = 0.5f;
            };
            fill();
            chain.process(e.outPtr[0], e.outPtr[1], kFrames);
            c.check(std::fabs(e.out[0][0] - 0.5f) < 1e-6f, "a plugin effect passes the sound while it loads");
            auto *effect = dynamic_cast<audio::PluginEffect *>(chain.effect(0));
            const auto until = Clock::now() + std::chrono::seconds(10);
            while (effect && effect->slot()->state() == plugin::Slot::State::Loading && Clock::now() < until) {
                chain.collect();
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            c.check(effect && effect->slot()->state() == plugin::Slot::State::Ready && effect->slot()->params().size() == 4,
                    "its slot loads it, with its parameters", effect ? effect->slot()->message() : "");
            settings[0].setValue(1, -6.0206);
            chain.apply(settings);
            fill();
            chain.process(e.outPtr[0], e.outPtr[1], kFrames);
            c.check(std::fabs(e.out[0][10] - 0.25f) < 0.005f, "the project's values reach it", text("%.3f", e.out[0][10]));
            settings[0].values.clear(); // the undo of that change
            chain.apply(settings);
            for (int i = 0; i < 400 && effect->slot()->busy(); ++i) {
                chain.collect();
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            fill();
            chain.process(e.outPtr[0], e.outPtr[1], kFrames);
            c.check(std::fabs(e.out[0][10] - 0.5f) < 0.005f, "dropping a value brings back its state", text("%.3f", e.out[0][10]));
            auto captured = effect->slot()->captureState();
            c.check(captured && captured->find("gain=0.000000") == 0, "its state can be captured for saving", captured ? *captured : "");
            const auto texts = Clock::now() + std::chrono::seconds(2);
            std::string shown = effect->slot()->text(1, -12.0);
            while (shown != "-12.0 dB" && Clock::now() < texts) {
                chain.collect();
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                shown = effect->slot()->text(1, -12.0);
            }
            c.check(shown == "-12.0 dB", "value texts come from the plugin, in the background", shown);
            // Automation: a curve on Gain reaches the plugin as a value event in the block, not through
            // the project's values (which still say 0 dB)
            {
                audio::AutomationCurve curve;
                curve.param = 1;
                curve.curve.min = -60.0;
                curve.curve.max = 12.0;
                curve.points = {{0.0, static_cast<float>((-6.0206 + 60.0) / 72.0)}};
                chain.setAutomation({{effect, {curve}}}, {});
                audio::TransportState transport;
                transport.sampleRate = kRate;
                fill();
                chain.process(e.outPtr[0], e.outPtr[1], kFrames, &transport);
                c.check(std::fabs(e.out[0][10] - 0.25f) < 0.005f, "automation reaches it as a value in the block",
                        text("%.3f", e.out[0][10]));
                chain.setAutomation({}, {});
            }
            settings[0].bypass = true;
            chain.apply(settings);
            fill();
            chain.process(e.outPtr[0], e.outPtr[1], kFrames);
            chain.process(e.outPtr[0], e.outPtr[1], kFrames);
            fill();
            chain.process(e.outPtr[0], e.outPtr[1], kFrames);
            c.check(std::fabs(e.out[0][10] - 0.5f) < 1e-6f, "bypassed, it is skipped");
        }
        {
            audio::DeviceChain chain;
            chain.setPlugins(&manager);
            chain.prepare(kRate, kFrames);
            std::vector<audio::DeviceSetting> settings = {pluginDevice("vybn.test.faulty", "Test Faulty")};
            chain.apply(settings);
            auto *effect = dynamic_cast<audio::PluginEffect *>(chain.effect(0));
            const auto until = Clock::now() + std::chrono::seconds(10);
            while (effect->slot()->state() == plugin::Slot::State::Loading && Clock::now() < until) {
                chain.collect();
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            settings[0].setValue(1, 1.0); // crash
            chain.apply(settings);
            Buffers e;
            for (uint32_t i = 0; i < kFrames; ++i)
                e.out[0][i] = e.out[1][i] = 0.5f;
            chain.process(e.outPtr[0], e.outPtr[1], kFrames);
            for (int i = 0; i < 200 && effect->slot()->state() == plugin::Slot::State::Ready; ++i) {
                manager.tick(environment);
                chain.collect();
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            c.check(effect->slot()->state() == plugin::Slot::State::Crashed && e.peak(0) == 0.0f, "a crashed effect is silent and says so",
                    effect->slot()->message());
            settings[0].values.clear();
            chain.apply(settings);
            effect->slot()->reload();
            for (int i = 0; i < 400 && effect->slot()->state() != plugin::Slot::State::Ready; ++i) {
                chain.collect();
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            for (uint32_t i = 0; i < kFrames; ++i)
                e.out[0][i] = e.out[1][i] = 0.5f;
            chain.process(e.outPtr[0], e.outPtr[1], kFrames);
            c.check(effect->slot()->state() == plugin::Slot::State::Ready && e.peak(0) > 0.4f, "Reload brings it back");
        }
        // A plugin with a sidechain port in a chain: Test Duck ducks its input by the key's peak in the
        // block. The chain is handed a key before each process, as a TrackProcessor does for a keyed
        // effect; no key, or a silent one, passes the sound; the key doesn't linger into the next block.
        {
            audio::DeviceChain chain;
            chain.setPlugins(&manager);
            chain.prepare(kRate, kFrames);
            chain.apply({pluginDevice("vybn.test.duck", "Test Duck")});
            auto *effect = dynamic_cast<audio::PluginEffect *>(chain.effect(0));
            const auto until = Clock::now() + std::chrono::seconds(10);
            while (effect && effect->slot()->state() == plugin::Slot::State::Loading && Clock::now() < until) {
                chain.collect();
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            Buffers e;
            std::vector<float> keyLeft(kFrames, 0.0f), keyRight(kFrames, 0.0f);
            auto fill = [&e] {
                for (uint32_t i = 0; i < kFrames; ++i)
                    e.out[0][i] = e.out[1][i] = 0.5f;
            };
            auto run = [&](const float *l, const float *r) {
                fill();
                if (l)
                    chain.setKey(0, l, r);
                chain.process(e.outPtr[0], e.outPtr[1], kFrames);
                return e.out[0][100];
            };
            const double unkeyed = run(nullptr, nullptr);
            std::fill(keyLeft.begin(), keyLeft.end(), 0.75f);
            std::fill(keyRight.begin(), keyRight.end(), 0.0f);
            const double keyedLoud = run(keyLeft.data(), keyRight.data()); // left channel at 0.75: gain 0.25
            std::fill(keyLeft.begin(), keyLeft.end(), 1.0f);
            const double keyedFull = run(keyLeft.data(), keyRight.data());
            std::fill(keyLeft.begin(), keyLeft.end(), 0.0f);
            std::fill(keyRight.begin(), keyRight.end(), 0.5f);
            const double keyedRight = run(keyLeft.data(), keyRight.data()); // the right channel counts too
            std::fill(keyRight.begin(), keyRight.end(), 0.0f);
            const double silentKey = run(keyLeft.data(), keyRight.data());
            const double afterwards = run(nullptr, nullptr); // the key doesn't linger
            c.check(effect && effect->slot()->state() == plugin::Slot::State::Ready && std::fabs(unkeyed - 0.5) < 1e-6 &&
                        std::fabs(silentKey - 0.5) < 1e-6 && std::fabs(afterwards - 0.5) < 1e-6,
                    "a plugin with a sidechain port passes its input with no key or a silent one",
                    text("%.3f unkeyed, ", unkeyed) + text("%.3f silent key", silentKey));
            c.check(std::fabs(keyedLoud - 0.125) < 1e-4 && std::fabs(keyedFull) < 1e-6 && std::fabs(keyedRight - 0.25) < 1e-4,
                    "and is ducked by the key it is given (both channels of it)",
                    text("%.4f at 0.75, ", keyedLoud) + text("%.4f at full scale, ", keyedFull) + text("%.4f at 0.5 on the right", keyedRight));
            // A plugin without a sidechain port ignores the key
            chain.apply({pluginDevice("vybn.test.gain", "Test Gain")});
            auto *plain = dynamic_cast<audio::PluginEffect *>(chain.effect(0));
            for (int i = 0; i < 1000 && plain && plain->slot()->state() == plugin::Slot::State::Loading; ++i) {
                chain.collect();
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            std::fill(keyLeft.begin(), keyLeft.end(), 1.0f);
            const double ignored = run(keyLeft.data(), keyRight.data());
            c.check(plain && std::fabs(ignored - 0.5) < 0.01, "a plugin with no sidechain port ignores the key", text("%.3f", ignored));
        }
        // The same through the instance: the block's key reaches the sidechain port, and the round trip
        // costs about the same keyed and unkeyed
        {
            std::string why;
            std::unique_ptr<plugin::Instance> duck = plugin::Instance::create(duckD, sandbox, why);
            c.check(duck && duck->activate(kRate, why), "Test Duck loads and activates on its own", why);
            if (duck) {
                Buffers d;
                for (uint32_t i = 0; i < kFrames; ++i)
                    d.in[0][i] = d.in[1][i] = 0.5f;
                std::vector<float> k0(kFrames, 0.5f), k1(kFrames, 0.5f);
                const float *keys[2] = {k0.data(), k1.data()};
                auto block = [&](bool keyed) {
                    plugin::ProcessBlock b = d.block(true);
                    b.keys = keyed ? keys : nullptr;
                    b.keyChannels = keyed ? 2 : 0;
                    return b;
                };
                bool answered = duck->process(block(false)) && std::fabs(d.out[0][10] - 0.5f) < 1e-6f;
                const bool ducked = duck->process(block(true)) && std::fabs(d.out[0][10] - 0.25f) < 1e-4f;
                const bool back = duck->process(block(false)) && std::fabs(d.out[0][10] - 0.5f) < 1e-6f;
                c.check(answered && ducked && back, "the instance hands the block's key to the sidechain port, and only for that block");
                auto time = [&](bool keyed) {
                    const int blocks = 400;
                    const auto t0 = Clock::now();
                    for (int i = 0; i < blocks; ++i)
                        duck->process(block(keyed));
                    return since(t0) / blocks * 1e6;
                };
                time(false); // warm
                const double plain = time(false), keyed = time(true);
                std::printf("INFO plugin sidechain: a block's round trip %.0f us unkeyed, %.0f us keyed (a plain gain plugin: see above)\n", plain, keyed);
                c.check(keyed < 2000.0 && duck->missed() == 0, "a keyed block's round trip is quick too",
                        text("%.0f us unkeyed, ", plain) + text("%.0f us keyed", keyed));
            }
        }
        // The scan's sidechain info, as the Sidechain control and the rules read it: Test Duck has a
        // stereo sidechain port, Test Gain, the sine and the faulty one none
        {
            plugin::Descriptor duck, gainInfo;
            const bool foundDuck = manager.find("vybn.test.duck", duck), foundGain = manager.find("vybn.test.gain", gainInfo);
            plugin::Descriptor crashy;
            const bool foundCrashy = manager.find("vybn.test.crashinit", crashy);
            c.check(foundCrashy && crashy.sidechain == 0 && crashy.probed && manager.lastScan().probed == 7,
                    "a plugin that crashes when made is still listed by the scan; its failed probe leaves no sidechain and is remembered",
                    text("%.0f probed", static_cast<double>(manager.lastScan().probed)));
            c.check(foundDuck && foundGain && duck.sidechain == 2 && gainInfo.sidechain == 0 && !duck.instrument(),
                    "the scan reports a plugin's sidechain port (channels), and none for a plugin without one",
                    text("duck %.0f, ", foundDuck ? duck.sidechain : 99) + text("gain %.0f", foundGain ? gainInfo.sidechain : 99));
            daw::setPluginSidechainLookup([&manager](const std::string &id) -> uint32_t {
                plugin::Descriptor d;
                return manager.find(id, d) ? d.sidechain : 0u;
            });
            daw::Project p;
            p.tracks.resize(2);
            p.tracks[0].name = "Pad";
            p.tracks[1].name = "Kick";
            p.tracks[0].effects = {pluginDevice("vybn.test.duck", "Test Duck"), pluginDevice("vybn.test.gain", "Test Gain"),
                                   pluginDevice("vybn.not.installed", "Missing")};
            p.tracks[0].effects[2].sidechain = 2; // a key a missing plugin keeps
            daw::ObjectId next = 1;
            p.normalizeIds(next);
            const daw::SidechainOffer duckOffer = daw::sidechainOffer(p, daw::DevicePlace::effect(false, 0, 0));
            const daw::SidechainOffer gainOffer = daw::sidechainOffer(p, daw::DevicePlace::effect(false, 0, 1));
            const daw::SidechainOffer missing = daw::sidechainOffer(p, daw::DevicePlace::effect(false, 0, 2));
            c.check(duckOffer.available && duckOffer.sources.size() == 1 && !gainOffer.available && !missing.available &&
                        p.tracks[0].effects[2].sidechain == p.tracks[1].id,
                    "a plugin with a sidechain port is offered the control, one without or not installed isn't (and keeps its key)");
            // The rules count every keyed plugin, installed or not, so the limit holds whatever the engine finds
            c.check(daw::keyedCount(p.tracks[0].effects) == 1 && daw::routingError(p).empty() && !daw::takesSidechain(p.tracks[0].effects[2]) &&
                        daw::mayTakeSidechain(p.tracks[0].effects[2]),
                    "a keyed plugin that isn't installed still counts toward the limit and the file keeps it");
            const daw::RoutingPlan plan = daw::planRouting(p, {0, 0}, 16384);
            c.check(plan.nodes[0].keys.size() == 1 && plan.nodes[0].keys[0].effect == 2 && plan.nodes[1].stage < plan.nodes[0].stage,
                    "and the plan orders and times its key like any other");
            daw::setPluginSidechainLookup({});
        }

        // A plugin keyed through the bridge: a pad with Test Duck, keyed by a muted kick track
        {
            const fs::path dir = fs::temp_directory_path() / "vybn-plugin-sidechain";
            std::error_code removed;
            fs::remove_all(dir, removed);
            fs::create_directories(dir, removed);
            daw::Settings settings;
            settings.libraryPath = dir.u8string();
            daw::InstrumentLibrary library(environment, settings);
            daw::AudioPool pool(environment);
            audio::AudioSystem engine(environment, audio::Deviceless{});
            engine.masterGain().set(1.0f);
            audio::MidiHub &midi = engine.add<audio::MidiHub>();
            daw::AudioBridge bridge(engine, midi, &manager, library, pool);
            daw::AudioBridge::SyncOptions options;
            options.listen = false;

            daw::Project p;
            p.tracks.resize(2);
            for (daw::Track &t : p.tracks) {
                t.kind = daw::TrackKind::Instrument;
                t.instrument = audio::SynthDevice::setting();
            }
            p.tracks[0].name = "Pad";
            p.tracks[0].clips.push_back(daw::Clip("Held", 0.0, 16.0, {daw::Note{0.0, 16.0, 57, 0.8f}}));
            p.tracks[0].effects = {pluginDevice("vybn.test.duck", "Test Duck")};
            p.tracks[1].name = "Kick";
            std::vector<daw::Note> beats;
            for (int beat = 0; beat < 4; ++beat)
                beats.push_back(daw::Note{static_cast<double>(beat), 0.25, 36, 0.9f});
            p.tracks[1].clips.push_back(daw::Clip("Kick", 0.0, 16.0, beats));
            p.tracks[1].mute = true;
            p.tracks[1].gain = 0.0f;
            audio::DeviceSetting boost = audio::effectSetting(audio::EffectType::Gain);
            boost.setValue(0, 24.0);
            p.tracks[1].effects = {boost};
            daw::ObjectId next = 1;
            p.normalizeIds(next);
            const daw::ObjectId kick = p.tracks[1].id;
            daw::ProjectStore store(p);
            bridge.sync(store, options);
            // The plugin loads in the background: sync until its slot is ready
            plugin::Slot *slot = nullptr;
            const auto until = Clock::now() + std::chrono::seconds(15);
            while (Clock::now() < until) {
                bridge.sync(store, options);
                audio::DeviceChain *chain = bridge.chainOf(false, 0);
                audio::Effect *effect = chain ? chain->effect(0) : nullptr;
                slot = effect ? effect->slot() : nullptr;
                if (slot && slot->state() == plugin::Slot::State::Ready)
                    break;
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            c.check(slot && slot->state() == plugin::Slot::State::Ready, "Test Duck loads on a track through the bridge");
            // The pad's level in dB while a kick note sounds and between the notes, over beats 1 to 3
            struct Levels {
                double during = 0.0, between = 0.0;
            };
            auto rms = [](const std::vector<float> &stereo, size_t from, size_t to) {
                double sum = 0.0;
                size_t n = 0;
                for (size_t f = from; f < to && f * 2 + 1 < stereo.size(); ++f, n += 2)
                    sum += static_cast<double>(stereo[f * 2]) * stereo[f * 2] + static_cast<double>(stereo[f * 2 + 1]) * stereo[f * 2 + 1];
                return n ? std::sqrt(sum / static_cast<double>(n)) : 0.0;
            };
            auto levels = [&] {
                bridge.sync(store, options);
                constexpr uint32_t block = 512;
                engine.transport().setTempo(120.0);
                engine.transport().locate(0);
                engine.transport().play();
                std::vector<float> out(static_cast<size_t>(4.0 * 24000.0) * 2);
                for (size_t at = 0; at + block * 2 <= out.size(); at += block * 2)
                    engine.renderOffline(out.data() + at, block, 48000.0);
                engine.transport().stop();
                double during = 0.0, between = 0.0;
                for (size_t beat = 1; beat < 4; ++beat) {
                    during += rms(out, beat * 24000 + 1500, beat * 24000 + 4500);
                    between += rms(out, beat * 24000 + 18000, beat * 24000 + 23000);
                }
                Levels l;
                l.during = 20.0 * std::log10(std::max(during / 3.0, 1e-9));
                l.between = 20.0 * std::log10(std::max(between / 3.0, 1e-9));
                return l;
            };
            const Levels unkeyed = levels();
            store.editTrack(0).effects[0].sidechain = kick;
            const Levels keyed = levels();
            store.editTrack(0).effects[0].sidechain = daw::kNoId;
            const Levels released = levels();
            c.check(std::fabs(unkeyed.during - unkeyed.between) < 1.0 && unkeyed.between > -40.0,
                    "an unkeyed Test Duck passes the pad steadily",
                    text("%.1f dB during, ", unkeyed.during) + text("%.1f dB between", unkeyed.between));
            c.check(keyed.between - keyed.during > 20.0 && std::fabs(keyed.between - unkeyed.between) < 1.0,
                    "keyed by a muted kick through the bridge, it ducks the pad on each kick and not between",
                    text("%.1f dB during, ", keyed.during) + text("%.1f dB between", keyed.between));
            c.check(std::fabs(released.during - released.between) < 1.0 && std::fabs(released.between - unkeyed.between) < 1.0,
                    "and removing the key brings the steady level back",
                    text("%.1f dB during, ", released.during) + text("%.1f dB between", released.between));
        }

        // A keyed plugin that crashes: silence, no dangling key, and keyed again after Reload
        {
            audio::DeviceChain chain;
            chain.setPlugins(&manager);
            chain.prepare(kRate, kFrames);
            std::vector<audio::DeviceSetting> settings = {pluginDevice("vybn.test.faulty", "Test Faulty")};
            chain.apply(settings);
            auto *effect = dynamic_cast<audio::PluginEffect *>(chain.effect(0));
            const auto until = Clock::now() + std::chrono::seconds(10);
            while (effect->slot()->state() == plugin::Slot::State::Loading && Clock::now() < until) {
                chain.collect();
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            Buffers e;
            std::vector<float> keyLeft(kFrames, 0.5f), keyRight(kFrames, 0.5f);
            auto run = [&] {
                for (uint32_t i = 0; i < kFrames; ++i)
                    e.out[0][i] = e.out[1][i] = 0.5f;
                chain.setKey(0, keyLeft.data(), keyRight.data());
                chain.process(e.outPtr[0], e.outPtr[1], kFrames);
            };
            settings[0].setValue(1, 1.0); // crash
            chain.apply(settings);
            run();
            for (int i = 0; i < 200 && effect->slot()->state() == plugin::Slot::State::Ready; ++i) {
                manager.tick(environment);
                chain.collect();
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            run(); // keyed blocks keep coming while it is down
            run();
            c.check(effect->slot()->state() == plugin::Slot::State::Crashed && e.peak(0) == 0.0f,
                    "a keyed plugin that crashes is silent and says so, and keyed blocks go on", effect->slot()->message());
            settings[0].values.clear();
            chain.apply(settings);
            effect->slot()->reload();
            for (int i = 0; i < 400 && effect->slot()->state() != plugin::Slot::State::Ready; ++i) {
                chain.collect();
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            run();
            c.check(effect->slot()->state() == plugin::Slot::State::Ready && e.peak(0) > 0.4f, "and after Reload it plays keyed blocks again");
        }
        // A plugin instrument on a track's note player: sequenced notes start on their frame in
        // one block (no splitting), and switching back to the sampler lets its notes go
        {
            // A track's chain with the plugin as its instrument, played through a ChainInstrument
            audio::DeviceChain chain;
            chain.setPlugins(&manager);
            chain.prepare(kRate, kFrames);
            auto owned = std::make_unique<audio::ChainInstrument>();
            owned->setChain(&chain);
            audio::NotePlayer player(std::move(owned));
            player.prepare(kRate, kFrames);
            chain.apply(pluginDevice("vybn.test.sine", "Test Sine"), {});
            auto *sound = dynamic_cast<audio::PluginInstrument *>(chain.instrument());
            const auto until = Clock::now() + std::chrono::seconds(10);
            while (sound && sound->slot() && sound->slot()->state() == plugin::Slot::State::Loading && Clock::now() < until) {
                chain.collect();
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            c.check(sound && sound->slot() && sound->slot()->state() == plugin::Slot::State::Ready && sound->params().size() == 1,
                    "a track's plugin instrument loads, as a device with its parameters",
                    sound && sound->slot() ? sound->slot()->message() : "");
            auto sequence = std::make_unique<audio::NoteSequence>();
            audio::SequencedNote note;
            note.start = 300.0 / 24000.0; // frame 300 at 120 BPM and 48 kHz
            note.end = 1.0;
            note.key = 69;
            note.velocity = 1.0f;
            sequence->notes.push_back(note);
            player.setSequence(std::move(sequence));
            Buffers s;
            audio::TransportState t;
            t.playing = true;
            t.position = 0;
            t.tempo = 120.0;
            t.sampleRate = kRate;
            for (int ch = 0; ch < 2; ++ch)
                std::fill(s.out[ch].begin(), s.out[ch].end(), 0.0f);
            audio::AudioBlock block{s.outPtr, 2, kFrames, kRate, t};
            player.process(block);
            c.check(s.peak(0, 0, 300) == 0.0f && s.peak(0, 300, kFrames) > 0.3f, "its sequenced notes start on their frame",
                    text("peak after %.2f", s.peak(0, 300, kFrames)));
            // A note's tuning curve through the note player: a fifth up from its start
            {
                auto tuned = std::make_unique<audio::NoteSequence>();
                audio::SequencedNote bent = note;
                bent.start = 0.0;
                bent.end = 8.0;
                bent.curveCount = 1;
                tuned->notes.push_back(bent);
                audio::ExpressionCurve curve;
                curve.points = {{0.0, 7.0f}};
                tuned->curves.push_back(curve);
                audio::NotePlayer bender(std::make_unique<audio::PluginInstrument>(&manager, pluginDevice("vybn.test.sine", "Test Sine")));
                bender.prepare(kRate, kFrames);
                auto *own = dynamic_cast<audio::PluginInstrument *>(&bender.instrument());
                const auto loaded = Clock::now() + std::chrono::seconds(10);
                while (own && own->slot() && own->slot()->state() == plugin::Slot::State::Loading && Clock::now() < loaded) {
                    bender.collect();
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                }
                bender.setSequence(std::move(tuned));
                audio::TransportState from = t;
                int n = 0;
                float before = 0.0f;
                for (int blockIndex = 0; blockIndex < 12; ++blockIndex) {
                    for (int ch = 0; ch < 2; ++ch)
                        std::fill(s.out[ch].begin(), s.out[ch].end(), 0.0f);
                    audio::AudioBlock part{s.outPtr, 2, kFrames, kRate, from};
                    bender.process(part);
                    from.position += kFrames;
                    for (uint32_t i = 0; blockIndex >= 2 && i < kFrames; ++i) {
                        if ((before < 0.0f) != (s.out[0][i] < 0.0f))
                            ++n;
                        before = s.out[0][i];
                    }
                    if (blockIndex < 2)
                        before = s.out[0][kFrames - 1];
                }
                const double hz = n / 2.0 / (10.0 * kFrames / kRate);
                const double fifth = 440.0 * std::pow(2.0, 7.0 / 12.0);
                c.check(std::fabs(hz - fifth) < 12.0, "a note's tuning curve plays through the note player",
                        text("%.1f Hz, not %.1f", hz, fifth));
            }
            // Stopping the transport lets the note go (by key: the sine ignores a wildcard note off)
            audio::TransportState stopped = t;
            stopped.playing = false;
            stopped.position = kFrames;
            for (int i = 0; i < 3; ++i) {
                for (int ch = 0; ch < 2; ++ch)
                    std::fill(s.out[ch].begin(), s.out[ch].end(), 0.0f);
                audio::AudioBlock next{s.outPtr, 2, kFrames, kRate, stopped};
                player.process(next);
            }
            c.check(s.peak(0) == 0.0f, "stopping the transport lets the plugin's notes go", text("peak %.3f", s.peak(0)));
            // Played again from the start, then switched to the sampler
            for (int ch = 0; ch < 2; ++ch)
                std::fill(s.out[ch].begin(), s.out[ch].end(), 0.0f);
            audio::AudioBlock again{s.outPtr, 2, kFrames, kRate, t};
            player.process(again);
            c.check(s.peak(0, 300, kFrames) > 0.3f, "it plays again after a stop", text("peak %.2f", s.peak(0, 300, kFrames)));
            // Another instrument in its place (a library one, here with no samples: silent)
            chain.apply(audio::SamplerDevice::setting("Nothing"), {});
            c.check(dynamic_cast<audio::SamplerDevice *>(chain.instrument()) != nullptr, "another instrument takes its place in the chain");
            audio::TransportState t2 = t;
            t2.position = kFrames;
            for (int i = 0; i < 3; ++i) {
                for (int ch = 0; ch < 2; ++ch)
                    std::fill(s.out[ch].begin(), s.out[ch].end(), 0.0f);
                audio::AudioBlock next{s.outPtr, 2, kFrames, kRate, t2};
                player.process(next);
                t2.position += kFrames;
            }
            c.check(s.peak(0) == 0.0f, "the instrument it replaced falls silent at once");
        }
        // Latency: a plugin delaying its sound reports it (after the restart it asks for), and a
        // track without it, compensated, lines up with it: a click on both lands on the same frame
        {
            struct Click : audio::AudioProcessor {
                Click() : AudioProcessor("Click") {}
                bool done = false;
                void process(audio::AudioBlock &block) override
                {
                    if (!done)
                        for (uint32_t ch = 0; ch < block.channelCount; ++ch)
                            block.channels[ch][0] += 1.0f;
                    done = true;
                }
            };
            audio::TrackProcessor late("Late", nullptr, std::make_unique<Click>()), early("Early", nullptr, std::make_unique<Click>());
            late.chain.setPlugins(&manager);
            late.prepare(kRate, kFrames);
            early.prepare(kRate, kFrames);
            std::vector<audio::DeviceSetting> settings = {pluginDevice("vybn.test.gain", "Test Gain")};
            settings[0].setValue(4, 256.0);
            late.chain.apply(settings);
            auto *effect = dynamic_cast<audio::PluginEffect *>(late.chain.effect(0));
            audio::TransportState t;
            // Settle: loaded, the latency taken up, before any click is let through
            const auto until = Clock::now() + std::chrono::seconds(10);
            while (effect && effect->slot()->latency() != 256 && Clock::now() < until) {
                manager.tick(environment);
                late.chain.collect();
                // Blocks through the chain alone, so the click waits
                Buffers s;
                late.chain.process(s.outPtr[0], s.outPtr[1], kFrames, &t);
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            c.check(effect && effect->slot()->latency() == 256, "a plugin's latency comes across (after its restart)",
                    effect ? text("%.0f frames", effect->slot()->latency()) : "");
            const std::vector<uint32_t> delays = audio::compensations({effect ? effect->slot()->latency() : 0, 0});
            late.compensation = delays[0];
            early.compensation = delays[1];
            auto firstClick = [&](audio::TrackProcessor &track) {
                int found = -1;
                for (int b = 0; b < 4 && found < 0; ++b) {
                    Buffers out;
                    float *ch[2] = {out.out[0].data(), out.out[1].data()};
                    audio::AudioBlock block{ch, 2, kFrames, kRate, t};
                    track.process(block);
                    for (uint32_t i = 0; i < kFrames && found < 0; ++i)
                        if (std::fabs(out.out[0][i]) > 0.1f)
                            found = static_cast<int>(b * kFrames + i);
                }
                return found;
            };
            const int a = firstClick(late), b = firstClick(early);
            c.check(a == 256 && b == 256, "compensated, a click on both tracks lines up",
                    text("plugin track %.0f", a) + text(", other %.0f", b));
        }

        // The scan cache: a second scan loads nothing, a forced one everything, a changed file again
        {
            const fs::path temp = fs::temp_directory_path() / "vybn-plugin-cache-test";
            std::error_code ec;
            fs::remove_all(temp, ec);
            fs::create_directories(temp / "plugins", ec);
            fs::copy_file(fs::u8path(file), temp / "plugins" / "copy.clap", ec);
            plugin::PluginConfig cached;
            cached.folders = {(temp / "plugins").u8string()};
            cached.cacheFile = (temp / "plugins.cache").u8string();
            plugin::PluginManager scanner(environment, cached);
            auto scanNow = [&](bool force) {
                scanner.rescan(force);
                scanner.waitForScan(20000);
                return scanner.lastScan();
            };
            const plugin::ScanStats first = scanNow(false);
            const plugin::ScanStats second = scanNow(false);
            const plugin::ScanStats forced = scanNow(true);
            c.check(first.scanned == 1 && second.scanned == 0 && second.probed == 0 && second.cached == 1 && scanner.plugins().size() == 5 && forced.scanned == 1,
                    "the scan cache: unchanged files aren't loaded again",
                    text("%.0f scanned, then ", static_cast<double>(first.scanned)) + text("%.0f", static_cast<double>(second.scanned)));
            plugin::Descriptor cachedDuck;
            c.check(scanner.find("vybn.test.duck", cachedDuck) && cachedDuck.sidechain == 2,
                    "the scan cache keeps a plugin's sidechain port");
            // A cache from before the sidechain info (format 1) is read as nothing, so it is scanned again
            {
                const fs::path old = temp / "old.cache";
                std::ofstream(old, std::ios::binary) << "# VYBN plugin scans 1: old" << (char)10 << "file" << (char)9 << "x.clap" << (char)9 << "1" << (char)9 << "1" << (char)9 << (char)10;
                c.check(plugin::readScanCache(old.u8string()).empty(), "an older scan cache is ignored, so its plugins are scanned again");
            }
            // A file that changes is scanned again
            // (its size, since a host just let go of it may still hold it a moment)
            bool grown = false;
            for (int i = 0; i < 50 && !grown; ++i) {
                std::ofstream more(temp / "plugins" / "copy.clap", std::ios::binary | std::ios::app);
                more << "extra";
                grown = static_cast<bool>(more);
                if (!grown)
                    std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
            const plugin::ScanStats changed = scanNow(false);
            c.check(changed.scanned == 1, "a changed file is scanned again");
            // The cache's own format keeps awkward text
            std::vector<plugin::ScanRecord> records(1);
            records[0].path = "C:\\odd\tpath.clap";
            records[0].size = 42;
            records[0].modified = -7;
            records[0].error = "line one\nline two";
            plugin::Descriptor d;
            d.id = "x.y";
            d.name = "Tab\there";
            d.description = "back\\slash";
            records[0].plugins.push_back(d);
            const std::string cacheFile = (temp / "odd.cache").u8string();
            const bool wrote = plugin::writeScanCache(cacheFile, records);
            const auto back = plugin::readScanCache(cacheFile);
            c.check(wrote && back.size() == 1 && back[0].path == records[0].path && back[0].size == 42 && back[0].modified == -7 &&
                        back[0].error == records[0].error && back[0].plugins.size() == 1 && back[0].plugins[0].name == d.name &&
                        back[0].plugins[0].description == d.description,
                    "the scan cache keeps tabs, lines and backslashes");
            fs::remove_all(temp, ec);
        }
        const auto standard = plugin::PluginManager::standardFolders();
        c.check(standard.size() == 4 && standard[0].find("CLAP") != std::string::npos && standard[2].find("VST3") != std::string::npos,
                "the standard CLAP and VST3 folders are known",
                standard.empty() ? "" : standard[0]);
    }

    return c.finish("plugin");
}

}
