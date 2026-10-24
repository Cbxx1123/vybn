// DawApp: warping audio clips (docs/design/warping-plan.md): the audio editor's warp tools, the
// clip menu's, finding a clip's tempo, and loops that come in warped

#include "daw/app/DawApp.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <functional>
#include <vector>

#include "daw/model/Warping.h"
#include "daw/views/AudioEditor.h"
#include "daw/widgets/Format.h"
#include "audio/AudioFile.h"
#include "audio/Onsets.h"

namespace daw {

using namespace ui;

namespace {

constexpr double kMaxLoopSeconds = 60.0; // longer files never come in warped

std::string bpmText(double bpm)
{
    // Whole tempos without a fraction, others to two places
    return std::fabs(bpm - std::round(bpm)) < 0.005 ? format("%.0f", bpm) : format("%.2f", bpm);
}

}

const Clip *DawApp::warpClip() const
{
    const Track *t = project().track(project().selectedTrack);
    const Clip *c = project().clip();
    return t && t->kind == TrackKind::Audio && c && c->hasAudio() ? c : nullptr;
}

void DawApp::editWarp(const std::function<void(Clip &)> &change)
{
    if (!warpClip())
        return;
    change(session_.store().editClip(project().selectedTrack, project().selectedClip));
}

void DawApp::setWarp(audio::WarpMode mode)
{
    const audio::TempoMap &timing = project().timing;
    editWarp([&](Clip &c) { setWarpMode(c, timing, mode, timing.bpmAt(c.start)); });
}

void DawApp::detectWarpTempo()
{
    const Clip *c = warpClip();
    if (!c)
        return;
    tempoPending_ = c->id;
    if (!pool_->onsets(c->audioFile))
        notify("Listening for the clip's beats...");
    updateWarp();
}

void DawApp::updateWarp()
{
    if (tempoPending_ == kNoId)
        return;
    // The clip asked about, if it is still the selected one
    const Clip *c = warpClip();
    if (!c || c->id != tempoPending_) {
        tempoPending_ = kNoId;
        return;
    }
    const FileOnsets *onsets = pool_->onsets(c->audioFile);
    if (!onsets)
        return;
    tempoPending_ = kNoId;
    const audio::TempoMap &timing = project().timing;
    const double seconds = clipFileSeconds(timing, *c, c->end()) - c->offset;
    double bpm = audio::tempoFromLength(seconds, onsets->tempo);
    if (bpm <= 0.0)
        bpm = onsets->tempo;
    if (bpm <= 0.0) {
        notify("No steady beat found in the clip");
        return;
    }
    editWarp([&](Clip &edited) {
        if (!edited.warped())
            setWarpMode(edited, timing, audio::WarpMode::Beats, bpm);
        else
            setWarpBpm(edited, bpm);
    });
    notify("The clip's tempo is " + bpmText(bpm) + " BPM");
}

std::vector<MenuItem> DawApp::warpModeMenu()
{
    std::vector<MenuItem> items;
    for (audio::WarpMode mode : {audio::WarpMode::Repitch, audio::WarpMode::Beats, audio::WarpMode::Tonal}) {
        items.push_back(MenuItem::toggle(audio::warpModeLabel(mode), [this, mode] {
            const Clip *c = warpClip();
            return c && c->warp.mode == mode;
        }, [this, mode] { setWarp(mode); }));
    }
    items.push_back(MenuItem::divider());
    items.push_back(MenuItem::toggle("Off", [this] {
        const Clip *c = warpClip();
        return c && !c->warped();
    }, [this] { setWarp(audio::WarpMode::Off); }));
    return items;
}

std::vector<MenuItem> DawApp::warpMenu()
{
    std::vector<MenuItem> items;
    items.push_back(MenuItem::item("Detect tempo", [this] { detectWarpTempo(); }));
    // The clip's audio as a whole number of bars
    std::vector<MenuItem> bars;
    for (int n : {1, 2, 4, 8, 16}) {
        bars.push_back(MenuItem::item(n == 1 ? "1 bar" : std::to_string(n) + " bars", [this, n] {
            const audio::TempoMap &timing = project().timing;
            editWarp([&](Clip &c) {
                const double bpm = tempoForBars(timing, c, n);
                if (bpm <= 0.0)
                    return;
                if (!c.warped())
                    setWarpMode(c, timing, audio::WarpMode::Beats, bpm);
                else
                    setWarpBpm(c, bpm);
            });
        }));
    }
    items.push_back(MenuItem::sub("Tempo from the clip's length", bars));
    // Every transient onto the grid
    std::vector<MenuItem> grids;
    const std::pair<const char *, double> choices[] = {{"Quarter notes", 1.0}, {"Eighth notes", 0.5}, {"Sixteenth notes", 0.25}};
    for (const auto &[name, grid] : choices) {
        const double g = grid;
        grids.push_back(MenuItem::item(name, [this, g] {
            const Clip *c = warpClip();
            if (!c || !c->warped())
                return;
            const FileOnsets *onsets = pool_->onsets(c->audioFile);
            if (!onsets) {
                notify("Still finding the clip's transients; try again in a moment");
                return;
            }
            const audio::TempoMap &timing = project().timing;
            std::vector<audio::WarpMarker> markers = snapTransients(timing, *c, onsets->transients, g);
            if (markers.empty()) {
                notify("No transients in the clip to snap");
                return;
            }
            editWarp([&](Clip &edited) { setWarpMarkers(edited, std::move(markers)); });
        }).when([this] {
            const Clip *c = warpClip();
            return c && c->warped();
        }));
    }
    items.push_back(MenuItem::sub("Snap transients to the grid", grids));
    items.push_back(MenuItem::item("Clear warp markers", [this] {
        editWarp([](Clip &c) { setWarpMarkers(c, {}); });
    }).when([this] {
        const Clip *c = warpClip();
        return c && !c->warp.markers.empty();
    }));
    items.push_back(MenuItem::divider());
    items.push_back(MenuItem::item("Use the clip's tempo for the song", [this] {
        const Clip *c = warpClip();
        if (!c || !c->warped())
            return;
        setTempoAt(c->start, audio::warpBpmAt(c->warp, c->offset));
    }).when([this] {
        const Clip *c = warpClip();
        return c && c->warped();
    }));
    return items;
}

void DawApp::buildWarpTools(Container &tools, const MenuButton &menuButton)
{
    tools.add<SmallToggle>("Warp", [this] {
        const Clip *c = warpClip();
        return c && c->warped();
    }, [this](bool on) { setWarp(on ? audio::WarpMode::Beats : audio::WarpMode::Off); });
    menuButton(tools, [this] {
        const Clip *c = warpClip();
        return std::string(audio::warpModeLabel(c && c->warped() ? c->warp.mode : audio::WarpMode::Beats));
    }, [this] { return warpModeMenu(); }, 74.0f);
    auto warped = [this] {
        const Clip *c = warpClip();
        return c && c->warped();
    };
    tools.add<ScrubField>("BPM", [this] {
        const Clip *c = warpClip();
        return c && c->warped() ? bpmText(c->warp.bpm) : std::string("-");
    }, [this](int steps, bool fine) {
        editWarp([&](Clip &c) {
            if (c.warped())
                setWarpBpm(c, c.warp.bpm + steps * (fine ? 0.01 : 1.0));
        });
    }, warped).setValueWidth(44.0f);
    tools.add<Button>([this] { editWarp([](Clip &c) { if (c.warped()) setWarpBpm(c, c.warp.bpm * 2.0); }); })
        .setLabel("x2")
        .setVariant(ButtonVariant::Flat)
        .setSize(Size::Small);
    tools.add<Button>([this] { editWarp([](Clip &c) { if (c.warped()) setWarpBpm(c, c.warp.bpm / 2.0); }); })
        .setLabel("/2")
        .setVariant(ButtonVariant::Flat)
        .setSize(Size::Small);
    tools.add<ScrubField>("Pitch", [this] {
        const Clip *c = warpClip();
        if (!c || (c->warp.mode != audio::WarpMode::Beats && c->warp.mode != audio::WarpMode::Tonal))
            return std::string("-");
        const float t = c->warp.transpose;
        return std::fabs(t - std::round(t)) < 0.005f ? format("%+.0f st", t) : format("%+.2f", t);
    }, [this](int steps, bool fine) {
        editWarp([&](Clip &c) {
            c.warp.transpose = std::round((c.warp.transpose + static_cast<float>(steps) * (fine ? 0.01f : 1.0f)) * 100.0f) / 100.0f;
            audio::tidyWarp(c.warp);
        });
    }, [this] {
        const Clip *c = warpClip();
        return c && (c->warp.mode == audio::WarpMode::Beats || c->warp.mode == audio::WarpMode::Tonal);
    }).setValueWidth(46.0f);
    menuButton(tools, [] { return std::string("Warp tools"); }, [this] { return warpMenu(); }, 0.0f);
}

void DawApp::autoWarp(Clip &clip, const audio::AudioData &data)
{
    // A loop: short, with a tempo in its name or a steady beat that fits whole bars
    if (!settings_.warpLoops || data.seconds() > kMaxLoopSeconds || data.seconds() < 0.5)
        return;
    bool named = false;
    double bpm = audio::bpmFromName(std::filesystem::u8path(clip.audioFile).stem().u8string(), &named);
    if (bpm > 0.0 && !named)
        bpm = audio::tempoFromLength(data.seconds(), bpm); // a bare number counts only when the length agrees
    if (bpm <= 0.0) {
        // Its beat, from all of it (a streamed file is decoded whole: it is a minute at most)
        std::shared_ptr<const audio::AudioData> whole;
        if (data.streamed()) {
            std::string error;
            whole = audio::loadAudioFile(data.path, error, nullptr);
        }
        const audio::AudioData &d = whole ? *whole : data;
        if (d.samples.empty())
            return;
        const audio::OnsetAnalysis onsets = audio::analyseOnsets(d.samples.data(), d.frames, d.channels, d.sampleRate);
        bpm = audio::tempoFromLength(d.seconds(), audio::estimateTempo(onsets));
    }
    if (bpm <= 0.0)
        return;
    setWarpMode(clip, project().timing, audio::WarpMode::Beats, bpm);
}

void DawApp::warpDemo()
{
    // Two bars of a beat at 96 BPM: a kick on 1 and 3, a snare on 2 and 4, hats on the eighths
    const double rate = 48000.0, bpm = 96.0;
    const double beat = 60.0 / bpm;
    const size_t frames = static_cast<size_t>(rate * beat * 8.0);
    std::vector<float> samples(frames * 2, 0.0f);
    uint32_t seed = 4321;
    auto noise = [&seed] {
        seed = seed * 1664525u + 1013904223u;
        return static_cast<float>(static_cast<double>(seed >> 8) / 16777216.0 * 2.0 - 1.0);
    };
    auto hit = [&](double at, double seconds, const std::function<float(double)> &voice) {
        const auto start = static_cast<size_t>(at * rate);
        for (size_t i = 0; start + i < frames && i < static_cast<size_t>(seconds * rate); ++i) {
            const float v = voice(static_cast<double>(i) / rate);
            samples[(start + i) * 2] += v;
            samples[(start + i) * 2 + 1] += v;
        }
    };
    for (int eighth = 0; eighth < 16; ++eighth) {
        const double at = eighth * beat / 2.0;
        if (eighth % 4 == 0)
            hit(at, 0.4, [](double t) { return static_cast<float>(0.7 * std::exp(-t * 9.0) * std::sin(6.2831853 * (50.0 * t + 60.0 * (1.0 - std::exp(-t * 30.0)) / 30.0))); });
        if (eighth % 4 == 2)
            hit(at, 0.25, [&noise](double t) { return static_cast<float>(0.45 * std::exp(-t * 18.0)) * noise(); });
        hit(at, 0.06, [&noise](double t) { return static_cast<float>(0.12 * std::exp(-t * 70.0)) * noise(); });
    }
    const std::string path = (std::filesystem::temp_directory_path() / "VYBN demo beat 96bpm.wav").string();
    std::string error;
    if (!audio::writeWavFile(path, samples.data(), frames, 2, rate, error)) {
        notify(error);
        return;
    }
    const std::vector<double> transients = audio::analyseOnsets(samples.data(), frames, 2, rate).transients;
    int track = -1;
    for (size_t i = 0; i < project().tracks.size() && track < 0; ++i) {
        if (project().tracks[i].kind == TrackKind::Audio)
            track = static_cast<int>(i);
    }
    ui_.post([this, path, track, transients] {
        importAudioFiles({path}, track, 0.0); // its name says 96 BPM: it comes in warped
        if (const Clip *c = warpClip()) {
            std::vector<audio::WarpMarker> markers = snapTransients(project().timing, *c, transients, 0.5);
            editWarp([&](Clip &edited) { setWarpMarkers(edited, std::move(markers)); });
        }
        showEditor();
    });
}

}
