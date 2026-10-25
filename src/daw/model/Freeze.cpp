#include "daw/model/Freeze.h"

#include <algorithm>
#include <filesystem>
#include <utility>

namespace daw {

namespace {

void mix(uint64_t &h, const void *data, size_t size)
{
    const auto *p = static_cast<const unsigned char *>(data);
    for (size_t i = 0; i < size; ++i) {
        h ^= p[i];
        h *= 1099511628211ull;
    }
}

template <typename T>
void mixValue(uint64_t &h, const T &value)
{
    mix(h, &value, sizeof value);
}

void mixText(uint64_t &h, const std::string &text)
{
    mixValue(h, text.size());
    mix(h, text.data(), text.size());
}

void mixDevice(uint64_t &h, const audio::DeviceSetting &d)
{
    mixValue(h, d.kind);
    mixText(h, d.id);
    mixValue(h, d.bypass);
    mixValue(h, d.sidechain);
    for (const audio::ModRoute &route : d.modulation) {
        mixValue(h, route.source);
        mixValue(h, route.param);
        mixValue(h, route.depth);
    }
    for (const auto &[id, value] : d.values) {
        mixValue(h, id);
        mixValue(h, value);
    }
    for (const audio::AutomationLane &lane : d.automation) {
        mixValue(h, lane.param);
        for (const audio::AutomationPoint &p : lane.points) {
            mixValue(h, p.beat);
            mixValue(h, p.value);
        }
    }
}

// What a track plays through its devices, before its fader
void mixSound(uint64_t &h, const Track &t)
{
    mixValue(h, t.kind);
    if (t.kind == TrackKind::Instrument)
        mixDevice(h, t.instrument);
    mixValue(h, t.effects.size());
    for (const audio::DeviceSetting &d : t.effects)
        mixDevice(h, d);
    mixValue(h, t.midiEffects.size());
    for (const audio::DeviceSetting &d : t.midiEffects)
        mixDevice(h, d);
    // Its modulators move its devices (their routes to its volume and pan play after the freeze)
    for (const audio::Modulator &m : t.modulators) {
        mixValue(h, m.id);
        mixDevice(h, m.setting);
    }
    mixValue(h, t.clips.size());
    for (const Clip &c : t.clips) {
        mixValue(h, c.start);
        mixValue(h, c.length);
        mixText(h, c.audioFile);
        mixValue(h, c.offset);
        mixValue(h, c.gain);
        mixValue(h, c.fadeIn);
        mixValue(h, c.fadeOut);
        const audio::Warp &w = c.warp;
        mixValue(h, w.on);
        if (w.on) {
            mixValue(h, w.mode);
            mixValue(h, w.bpm);
            mixValue(h, w.pitch);
            mixValue(h, w.preserve);
            mixValue(h, w.loop);
            mixValue(h, w.envelope);
            mixValue(h, w.grain);
            for (const audio::WarpMarker &m : w.markers) {
                mixValue(h, m.seconds);
                mixValue(h, m.beat);
            }
        }
        for (const Note &n : c.notes) {
            if (n.muted)
                continue;
            mixValue(h, n.start);
            mixValue(h, n.length);
            mixValue(h, n.key);
            mixValue(h, n.velocity);
        }
        for (const ControlLane &lane : c.controls) {
            mixValue(h, lane.controller);
            for (const ControlPoint &p : lane.points) {
                mixValue(h, p.beat);
                mixValue(h, p.value);
            }
        }
    }
}

}

uint64_t freezeHash(const Project &project, int track)
{
    const Track *t = project.track(track);
    if (!t)
        return 0;
    uint64_t h = 1469598103934665603ull;
    mixSound(h, *t);
    for (const audio::TempoPoint &p : project.timing.points()) {
        mixValue(h, p.beat);
        mixValue(h, p.bpm);
        mixValue(h, p.ramp);
    }
    // The tracks its effects listen to
    for (const audio::DeviceSetting &d : t->effects) {
        if (d.sidechain == kNoId)
            continue;
        if (const Track *key = project.findTrack(d.sidechain))
            mixSound(h, *key);
    }
    return h;
}

bool freezeCurrent(const Project &project, int track)
{
    const Track *t = project.track(track);
    return t && t->frozen() && canFreeze(*t) && t->freeze.hash == freezeHash(project, track);
}

bool flattenTrack(Project &project, int track)
{
    Track *t = project.track(track);
    if (!t || !t->frozen() || !canFreeze(*t))
        return false;
    Clip clip(std::filesystem::u8path(t->freeze.file).stem().u8string(), 0.0,
              std::max(1.0 / 64.0, project.timing.beatAt(t->freeze.seconds)));
    clip.audioFile = t->freeze.file;
    t->kind = TrackKind::Audio;
    t->instrument = {};
    t->effects.clear();
    t->takeLanes.clear();
    t->clips = {std::move(clip)};
    t->freeze = {};
    t->armed = false;
    if (project.selectedTrack == track)
        project.selectedClip = -1;
    return true;
}

int addBounce(Project &project, int track, const std::string &file, double seconds)
{
    Track *source = project.track(track);
    if (!source)
        return -1;
    Track bounced;
    bounced.kind = TrackKind::Audio;
    bounced.name = source->name + " (bounced)";
    bounced.output = source->output;
    Clip clip(std::filesystem::u8path(file).stem().u8string(), 0.0, std::max(1.0 / 64.0, project.timing.beatAt(seconds)));
    clip.audioFile = file;
    bounced.clips.push_back(std::move(clip));
    source->mute = true;
    const auto at = static_cast<size_t>(track) + 1;
    project.tracks.insert(project.tracks.begin() + static_cast<std::ptrdiff_t>(at), std::move(bounced));
    // The selection keeps pointing at the track it was on
    if (project.selectedTrack >= static_cast<int>(at))
        ++project.selectedTrack;
    return static_cast<int>(at);
}

}
