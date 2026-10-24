// Warping in the app (docs/design/warp-plan.md): the audio editor's warp row, the Warp menu, a
// marker's menu, the Quantise audio dialog and the song's tempo from a clip.
#include "daw/app/DawApp.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <string>
#include <vector>

#include "daw/model/Warp.h"
#include "daw/views/AudioEditor.h"
#include "daw/views/TransportBar.h"
#include "daw/widgets/Format.h"

namespace daw {

using namespace ui;

Clip *DawApp::editWarpClip()
{
    const Track *t = project().track(project().selectedTrack);
    if (!t || t->kind != TrackKind::Audio || !project().clip() || !project().clip()->hasAudio())
        return nullptr;
    return &session_.store().editClip(project().selectedTrack, project().selectedClip);
}

double DawApp::warpFileSeconds(const Clip &clip)
{
    const auto data = clip.hasAudio() ? pool_->get(clip.audioFile) : nullptr;
    return data ? data->seconds() : 0.0;
}

const audio::AudioAnalysis *DawApp::warpAnalysis(const Clip &clip)
{
    return clip.hasAudio() ? pool_->analysis(clip.audioFile).get() : nullptr;
}

namespace {

// The selected audio clip, to read
const Clip *shownClip(const Project &project)
{
    const Track *t = project.track(project.selectedTrack);
    const Clip *c = project.clip();
    return t && t->kind == TrackKind::Audio && c && c->hasAudio() ? c : nullptr;
}

}

void DawApp::buildWarpTools(Container &parent)
{
    auto &bar = parent.add<Container>(Direction::Row);
    bar.setAlign(Align::Center);
    auto warped = [this] {
        const Clip *c = shownClip(project());
        return c && c->warp.on;
    };
    auto stretching = [this] {
        const Clip *c = shownClip(project());
        return c && c->warp.on && c->warp.mode != audio::WarpMode::Repitch;
    };
    // A button opening a menu, its label following the clip
    auto menuButton = [this, &bar](std::function<std::string()> label, std::function<std::vector<MenuItem>()> items, float width) {
        Button &b = bar.add<Button>();
        Button *raw = &b;
        b.onClick([this, raw, items] {
            const Rect r = raw->rect();
            openContextMenu(Context::activeOr(gui_), items(), {r.x, r.y + r.h + 6.0f});
        });
        b.setLabel(label()).setSize(Size::Small).setMinSize({width, 0.0f});
        auto shown = std::make_shared<std::string>(label());
        bar.add<FrameHook>([raw, label, shown](const Input &) {
            std::string now = label();
            if (now != *shown) {
                *shown = now;
                raw->setLabel(now);
            }
        });
    };

    bar.add<SmallToggle>("Warp", warped, [this](bool on) {
        if (Clip *c = editWarpClip()) {
            const bool first = c->warp.bpm <= 0.0;
            setWarp(*c, project().timing, on, warpFileSeconds(*c));
            if (first && on) {
                if (const audio::AudioAnalysis *a = warpAnalysis(*c))
                    c->warp.mode = guessMode(*a);
            }
        }
    });
    menuButton([this] {
        const Clip *c = shownClip(project());
        return std::string(c ? warpModeName(c->warp.mode) : "Mode");
    }, [this] { return warpModeMenu(); }, 78.0f);
    bar.add<Divider>();
    bar.add<ScrubField>("Tempo", [this] {
        const Clip *c = shownClip(project());
        return c && c->warp.bpm > 0.0 ? format("%.1f", c->warp.bpm) : std::string("-");
    }, [this](int count, bool fine) {
        if (Clip *c = editWarpClip(); c && c->warp.on)
            setClipTempo(*c, c->warp.bpm + count * (fine ? 0.1 : 1.0));
    }, warped).setValueWidth(44.0f);
    bar.add<Button>([this] {
        if (Clip *c = editWarpClip(); c && c->warp.on)
            setClipTempo(*c, c->warp.bpm * 2.0);
    }).setLabel("x2").setVariant(ButtonVariant::Flat).setSize(Size::Small);
    bar.add<Button>([this] {
        if (Clip *c = editWarpClip(); c && c->warp.on)
            setClipTempo(*c, c->warp.bpm * 0.5);
    }).setLabel("/2").setVariant(ButtonVariant::Flat).setSize(Size::Small);
    bar.add<Button>([this] {
        const Clip *shown = shownClip(project());
        const audio::AudioAnalysis *a = shown ? warpAnalysis(*shown) : nullptr;
        if (!a) {
            notify(shown ? "The clip's sound is still being looked at" : "Select an audio clip");
            return;
        }
        if (a->bpm <= 0.0) {
            notify("No steady tempo was found in this sound");
            return;
        }
        if (Clip *c = editWarpClip())
            applyGuessedTempo(*c, project().timing, *a);
        notify(format("Found %.1f BPM", a->bpm) + (a->loop ? format(", a loop of %.0f bars", static_cast<double>(a->loopBars)) : ""));
    }).setLabel("Guess").setVariant(ButtonVariant::Flat).setSize(Size::Small);
    bar.add<Divider>();
    // Transposition: whole semitones, and cents
    bar.add<ScrubField>("Transpose", [this] {
        const Clip *c = shownClip(project());
        return c ? format("%+.0f st", std::round(c->warp.pitch)) : std::string("-");
    }, [this](int count, bool) {
        if (Clip *c = editWarpClip())
            c->warp.pitch = std::clamp(c->warp.pitch + count, -audio::kMaxWarpPitch, audio::kMaxWarpPitch);
    }, stretching).setValueWidth(44.0f);
    bar.add<ScrubField>("Detune", [this] {
        const Clip *c = shownClip(project());
        if (!c)
            return std::string("-");
        return format("%+.0f ct", std::round((c->warp.pitch - std::round(c->warp.pitch)) * 100.0));
    }, [this](int count, bool fine) {
        if (Clip *c = editWarpClip()) {
            const double p = c->warp.pitch + count * (fine ? 0.01 : 0.05);
            c->warp.pitch = std::clamp(std::round(p * 100.0) / 100.0, -audio::kMaxWarpPitch, audio::kMaxWarpPitch);
        }
    }, stretching).setValueWidth(40.0f);
    bar.add<Divider>();
    // The mode's own settings: Beats' slicing, the end of a slice and its decay; Tones' grain
    auto beats = [this] {
        const Clip *c = shownClip(project());
        return c && c->warp.on && c->warp.mode == audio::WarpMode::Beats;
    };
    menuButton([this] {
        const Clip *c = shownClip(project());
        return std::string("Slices ") + warpPreserveName(c ? c->warp.preserve : audio::WarpPreserve::Transients);
    }, [this, beats] {
        std::vector<MenuItem> items;
        for (audio::WarpPreserve p : {audio::WarpPreserve::Transients, audio::WarpPreserve::Quarter, audio::WarpPreserve::Eighth,
                                      audio::WarpPreserve::Sixteenth}) {
            items.push_back(MenuItem::toggle(warpPreserveName(p), [this, p] {
                const Clip *c = shownClip(project());
                return c && c->warp.preserve == p;
            }, [this, p] {
                if (Clip *c = editWarpClip())
                    c->warp.preserve = p;
            }).when(beats));
        }
        return items;
    }, 0.0f);
    menuButton([this] {
        const Clip *c = shownClip(project());
        return std::string("Ends ") + warpLoopName(c ? c->warp.loop : audio::WarpLoop::Off);
    }, [this, beats] {
        std::vector<MenuItem> items;
        for (audio::WarpLoop l : {audio::WarpLoop::Off, audio::WarpLoop::Forward, audio::WarpLoop::BackAndForth}) {
            items.push_back(MenuItem::toggle(warpLoopName(l), [this, l] {
                const Clip *c = shownClip(project());
                return c && c->warp.loop == l;
            }, [this, l] {
                if (Clip *c = editWarpClip())
                    c->warp.loop = l;
            }).when(beats));
        }
        return items;
    }, 0.0f);
    bar.add<ScrubField>("Decay", [this] {
        const Clip *c = shownClip(project());
        return c ? format("%.0f", c->warp.envelope) : std::string("-");
    }, [this](int count, bool fine) {
        if (Clip *c = editWarpClip())
            c->warp.envelope = std::clamp(c->warp.envelope + static_cast<float>(count) * (fine ? 1.0f : 5.0f), 0.0f, 100.0f);
    }, beats).setValueWidth(28.0f);
    bar.add<ScrubField>("Grain", [this] {
        const Clip *c = shownClip(project());
        return c ? format("%.0f", c->warp.grain) : std::string("-");
    }, [this](int count, bool fine) {
        if (Clip *c = editWarpClip())
            c->warp.grain = std::clamp(c->warp.grain + static_cast<float>(count) * (fine ? 1.0f : 5.0f), 0.0f, 100.0f);
    }, [this] {
        const Clip *c = shownClip(project());
        return c && c->warp.on && c->warp.mode == audio::WarpMode::Tones;
    }).setValueWidth(28.0f);
    bar.add<Spacer>();
    menuButton([] { return std::string("Warp"); }, [this] { return warpMenu(); }, 0.0f);
}

std::vector<MenuItem> DawApp::warpModeMenu()
{
    std::vector<MenuItem> items;
    for (audio::WarpMode m : {audio::WarpMode::Beats, audio::WarpMode::Tones, audio::WarpMode::Complex, audio::WarpMode::Repitch}) {
        items.push_back(MenuItem::toggle(warpModeName(m), [this, m] {
            const Clip *c = shownClip(project());
            return c && c->warp.on && c->warp.mode == m;
        }, [this, m] {
            if (Clip *c = editWarpClip()) {
                c->warp.mode = m;
                setWarp(*c, project().timing, true, warpFileSeconds(*c));
            }
        }));
    }
    return items;
}

std::vector<MenuItem> DawApp::warpMenu()
{
    auto has = [this] { return shownClip(project()) != nullptr; };
    auto warped = [this] {
        const Clip *c = shownClip(project());
        return c && c->warp.on;
    };
    std::vector<MenuItem> items{
        MenuItem::toggle("Warp", warped, [this] {
            if (Clip *c = editWarpClip()) {
                const bool first = c->warp.bpm <= 0.0;
                setWarp(*c, project().timing, !c->warp.on, warpFileSeconds(*c));
                if (first && c->warp.on) {
                    if (const audio::AudioAnalysis *a = warpAnalysis(*c))
                        c->warp.mode = guessMode(*a);
                }
            }
        }).when(has),
        MenuItem::sub("Warp mode", warpModeMenu()),
        MenuItem::divider(),
        MenuItem::item("Double tempo", [this] {
            if (Clip *c = editWarpClip(); c && c->warp.on)
                setClipTempo(*c, c->warp.bpm * 2.0);
        }).when(warped),
        MenuItem::item("Halve tempo", [this] {
            if (Clip *c = editWarpClip(); c && c->warp.on)
                setClipTempo(*c, c->warp.bpm * 0.5);
        }).when(warped),
        MenuItem::item("Guess tempo", [this] {
            const Clip *shown = shownClip(project());
            const audio::AudioAnalysis *a = shown ? warpAnalysis(*shown) : nullptr;
            if (!a || a->bpm <= 0.0) {
                notify(a ? "No steady tempo was found in this sound" : "The clip's sound is still being looked at");
                return;
            }
            if (Clip *c = editWarpClip())
                applyGuessedTempo(*c, project().timing, *a);
        }).when(has),
        MenuItem::item("Quantise audio...", [this] { openQuantiseAudio(); }).when(has),
        MenuItem::item("Clear warp markers", [this] {
            if (Clip *c = editWarpClip(); c && c->warp.on)
                clearWarpMarkers(*c);
        }).when(warped),
        MenuItem::divider(),
        MenuItem::item("Set the song's tempo from this clip", [this] { setSongTempoFromClip(); }).when(has),
    };
    return items;
}

void DawApp::openWarpMarkerMenu(int marker, Vec2 at)
{
    std::vector<MenuItem> items{
        MenuItem::item("Delete marker", [this, marker] {
            if (Clip *c = editWarpClip())
                removeWarpMarker(*c, marker);
        }),
        MenuItem::item("Warp straight from here", [this, marker] {
            if (Clip *c = editWarpClip())
                warpStraightFrom(*c, marker);
        }),
        MenuItem::item("Move the playhead here", [this, marker] {
            const Clip *c = shownClip(project());
            if (c && marker >= 0 && static_cast<size_t>(marker) < c->warp.markers.size())
                seekBeats(audio_, c->start + c->warp.markers[static_cast<size_t>(marker)].beat - clipContentStart(*c));
        }),
        MenuItem::divider(),
        MenuItem::item("Clear warp markers", [this] {
            if (Clip *c = editWarpClip())
                clearWarpMarkers(*c);
        }),
    };
    openContextMenu(gui_, items, at);
}

void DawApp::openQuantiseAudio()
{
    const Clip *shown = shownClip(project());
    if (!shown) {
        notify("Select an audio clip to quantise");
        return;
    }
    Dialog &dialog = Dialog::open(gui_, "Quantise audio",
                                  "Warp markers at the clip's transients, moved toward the grid. The sound between them stretches.");
    dialog.setWidth(440.0f);
    Container &body = dialog.body();
    const std::vector<double> grids{1.0, 0.5, 0.25, 0.125};
    int chosen = 2;
    for (size_t i = 0; i < grids.size(); ++i) {
        if (std::fabs(grids[i] - warpQuantise_.grid) < 1e-9)
            chosen = static_cast<int>(i);
    }
    auto &gridRow = body.add<Container>(Direction::Row);
    gridRow.setAlign(Align::Center);
    gridRow.add<Label>("Grid", TextStyle::Body, TextTone::Secondary).setMinSize({96.0f, 0.0f});
    gridRow.add<SegmentedControl>(std::vector<std::string>{"1/4", "1/8", "1/16", "1/32"}, chosen, [this, grids](int i) {
        warpQuantise_.grid = grids[static_cast<size_t>(std::clamp(i, 0, 3))];
    }).setSize(Size::Small);
    auto slider = [&body](const char *label, float value, std::function<void(float)> set, std::function<std::string()> text) {
        auto &r = body.add<Container>(Direction::Row);
        r.setAlign(Align::Center);
        r.add<Label>(label, TextStyle::Body, TextTone::Secondary).setMinSize({96.0f, 0.0f});
        r.add<Slider>(value, std::move(set)).setSize(Size::Small).setGrow(1.0f);
        r.add<LiveLabel>(std::move(text), TextStyle::Caption, TextTone::Secondary).setMinSize({44.0f, 0.0f});
    };
    slider("Strength", warpQuantise_.strength, [this](float v) { warpQuantise_.strength = v; },
           [this] { return format("%.0f%%", warpQuantise_.strength * 100.0); });
    slider("Transients", 1.0f - warpQuantise_.threshold, [this](float v) { warpQuantise_.threshold = 1.0f - v; },
           [this] { return format("%.0f%%", (1.0f - warpQuantise_.threshold) * 100.0); });
    dialog.addButton("Cancel");
    dialog.addButton("Quantise", [this] {
        const Clip *c = shownClip(project());
        const audio::AudioAnalysis *a = c ? warpAnalysis(*c) : nullptr;
        if (!a) {
            notify("The clip's sound is still being looked at; try again in a moment");
            return;
        }
        int made = 0;
        if (Clip *edited = editWarpClip())
            made = quantiseAudio(*edited, project().timing, *a, warpQuantise_.grid, warpQuantise_.strength, warpQuantise_.threshold);
        notify(format("Quantised %.0f transient", static_cast<double>(made)) + (made == 1 ? "" : "s"));
    }, true);
}

void DawApp::setSongTempoFromClip()
{
    const Clip *c = shownClip(project());
    if (!c)
        return;
    double bpm = c->warp.on ? clipTempoAt(*c, c->start) : 0.0;
    if (bpm <= 0.0) {
        const audio::AudioAnalysis *a = warpAnalysis(*c);
        bpm = a ? a->bpm : 0.0;
    }
    if (bpm <= 0.0) {
        notify("No tempo is known for this clip yet");
        return;
    }
    // A warped clip then plays at its own speed; an unwarped one always did
    const double start = c->start;
    setTempoAt(start, std::round(bpm * 10.0) / 10.0);
    notify(format("The song's tempo is now %.1f BPM", std::round(bpm * 10.0) / 10.0));
}

}
