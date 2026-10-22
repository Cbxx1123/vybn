#include "daw/model/Project.h"

#include "daw/model/Routing.h"

#include <algorithm>
#include <set>

#include "audio/Effects.h"
#include "audio/PluginInstrument.h"

namespace daw {

Track *Project::track(int index)
{
    return index >= 0 && index < static_cast<int>(tracks.size()) ? &tracks[static_cast<size_t>(index)] : nullptr;
}

const Track *Project::track(int index) const
{
    return index >= 0 && index < static_cast<int>(tracks.size()) ? &tracks[static_cast<size_t>(index)] : nullptr;
}

int Project::trackIndex(ObjectId id) const
{
    if (id == kNoId)
        return -1;
    for (size_t i = 0; i < tracks.size(); ++i) {
        if (tracks[i].id == id)
            return static_cast<int>(i);
    }
    return -1;
}

Track *Project::findTrack(ObjectId id)
{
    return track(trackIndex(id));
}

const Track *Project::findTrack(ObjectId id) const
{
    return track(trackIndex(id));
}

bool Project::normalizeIds(ObjectId &next)
{
    // Above every id in use, so a fresh one never matches an existing one
    for (const Track &t : tracks) {
        next = std::max(next, t.id + 1);
        for (const Clip &c : t.clips)
            next = std::max(next, c.id + 1);
    }
    next = std::max<ObjectId>(next, 1);

    bool changed = false;
    std::set<ObjectId> trackIds;
    for (Track &t : tracks) {
        if (t.id == kNoId || trackIds.count(t.id)) {
            t.id = next++;
            changed = true;
        }
        trackIds.insert(t.id);
    }
    // Clip ids are unique across the whole project, not per track
    std::set<ObjectId> clipIds;
    for (Track &t : tracks) {
        for (Clip &c : t.clips) {
            if (c.id == kNoId || clipIds.count(c.id)) {
                c.id = next++;
                changed = true;
            }
            clipIds.insert(c.id);
        }
    }
    return changed;
}

std::vector<audio::DeviceSetting> *Project::effects(bool master, int index)
{
    if (master)
        return &masterEffects;
    Track *t = track(index);
    return t ? &t->effects : nullptr;
}

const std::vector<audio::DeviceSetting> *Project::effects(bool master, int index) const
{
    if (master)
        return &masterEffects;
    const Track *t = track(index);
    return t ? &t->effects : nullptr;
}

audio::DeviceSetting *Project::device(const DevicePlace &place)
{
    if (place.instrument()) {
        Track *t = track(place.track);
        return t && t->kind == TrackKind::Instrument && t->hasInstrument() ? &t->instrument : nullptr;
    }
    std::vector<audio::DeviceSetting> *chain = effects(place.master, place.track);
    return chain && place.index >= 0 && static_cast<size_t>(place.index) < chain->size() ? &(*chain)[static_cast<size_t>(place.index)]
                                                                                          : nullptr;
}

const audio::DeviceSetting *Project::device(const DevicePlace &place) const
{
    if (place.instrument()) {
        const Track *t = track(place.track);
        return t && t->kind == TrackKind::Instrument && t->hasInstrument() ? &t->instrument : nullptr;
    }
    const std::vector<audio::DeviceSetting> *chain = effects(place.master, place.track);
    return chain && place.index >= 0 && static_cast<size_t>(place.index) < chain->size() ? &(*chain)[static_cast<size_t>(place.index)]
                                                                                          : nullptr;
}

Clip *Project::clip()
{
    Track *t = track(selectedTrack);
    if (!t || selectedClip < 0 || selectedClip >= static_cast<int>(t->clips.size()))
        return nullptr;
    return &t->clips[static_cast<size_t>(selectedClip)];
}

const Clip *Project::clip() const
{
    const Track *t = track(selectedTrack);
    if (!t || selectedClip < 0 || selectedClip >= static_cast<int>(t->clips.size()))
        return nullptr;
    return &t->clips[static_cast<size_t>(selectedClip)];
}

void Project::select(int trackIndex, int clipIndex)
{
    selectedTrack = track(trackIndex) ? trackIndex : -1;
    selectedClip = selectedTrack >= 0 ? clipIndex : -1;
}

int Project::addTrack(TrackKind kind)
{
    int count = 1;
    for (const Track &t : tracks)
        count += t.kind == kind ? 1 : 0;
    Track t;
    t.kind = kind;
    // Returns are lettered (the mixer's send knobs show the letter): "Return A", "Return B"
    if (kind == TrackKind::Return)
        t.name = "Return " + returnLetter(count - 1);
    else
        t.name = std::string(kindName(kind)) + " " + std::to_string(count);
    tracks.push_back(t);
    select(static_cast<int>(tracks.size()) - 1);
    return selectedTrack;
}

void Project::deleteTrack(int index)
{
    if (!track(index))
        return;
    forgetTrack(*this, tracks[static_cast<size_t>(index)].id);
    tracks.erase(tracks.begin() + index);
    if (tracks.empty())
        select(-1);
    else
        select(std::min(index, static_cast<int>(tracks.size()) - 1));
}

void Project::moveTrack(int from, int to)
{
    if (from == to || !track(from) || !track(to))
        return;
    Track moved = std::move(tracks[static_cast<size_t>(from)]);
    tracks.erase(tracks.begin() + from);
    tracks.insert(tracks.begin() + to, std::move(moved));
    // The selection stays on the same track: it moves with the track, or shifts past the one that moved
    if (selectedTrack == from)
        selectedTrack = to;
    else if (from < selectedTrack && selectedTrack <= to)
        --selectedTrack;
    else if (to <= selectedTrack && selectedTrack < from)
        ++selectedTrack;
}

float ControlLane::valueAt(double beat) const
{
    if (points.empty())
        return controllerDefault(controller);
    if (beat <= points.front().beat)
        return points.front().value;
    if (beat >= points.back().beat)
        return points.back().value;
    const auto next = std::upper_bound(points.begin(), points.end(), beat,
                                       [](double b, const ControlPoint &p) { return b < p.beat; });
    const ControlPoint &b = *next;
    const ControlPoint &a = *(next - 1);
    if (controllerStepped(controller) || b.beat <= a.beat)
        return a.value;
    return a.value + (b.value - a.value) * static_cast<float>((beat - a.beat) / (b.beat - a.beat));
}

std::string controllerName(int controller)
{
    switch (controller) {
    case 1: return "Mod wheel";
    case 2: return "Breath";
    case 7: return "Volume";
    case 10: return "Pan";
    case 11: return "Expression";
    case 64: return "Sustain";
    case 71: return "Resonance";
    case 74: return "Brightness";
    case kPitchBend: return "Pitch bend";
    default: return "CC " + std::to_string(controller);
    }
}

bool controllerStepped(int controller) { return controller >= 64 && controller <= 69; }

float controllerDefault(int controller)
{
    switch (controller) {
    case kPitchBend:
    case 10: return 0.5f;
    case 7: return 100.0f / 127.0f;
    case 11: return 1.0f;
    default: return 0.0f;
    }
}

namespace {

// A clip's lanes cut at `at` beats: the part from there on, starting at 0 with the value there
// (and the left part ending with it)
std::vector<ControlLane> cutLanes(std::vector<ControlLane> &lanes, double at)
{
    std::vector<ControlLane> right;
    for (ControlLane &lane : lanes) {
        const float value = lane.valueAt(at);
        ControlLane after{lane.controller, {}};
        after.points.push_back({0.0, value});
        std::vector<ControlPoint> before;
        for (const ControlPoint &p : lane.points) {
            if (p.beat < at - 1e-9)
                before.push_back(p);
            else if (p.beat > at + 1e-9)
                after.points.push_back({p.beat - at, p.value});
        }
        before.push_back({at, value});
        lane.points = std::move(before);
        right.push_back(std::move(after));
    }
    return right;
}

}

void Project::toggleLoop()
{
    if (hasLoop()) {
        looping = !looping;
        return;
    }
    if (const Clip *c = clip()) {
        loopStart = c->start;
        loopEnd = c->start + c->length;
    } else {
        loopStart = 0.0;
        loopEnd = timing.barStart(4);
    }
    looping = true;
}

bool Project::splitClip(double beat)
{
    Clip *c = clip();
    if (!c || beat <= c->start + 1e-6 || beat >= c->start + c->length - 1e-6)
        return false;
    const double at = beat - c->start; // beats into the clip
    Clip right = *c;
    right.start = beat;
    right.length = c->length - at;
    right.offset = c->offset + timing.secondsAt(beat) - timing.secondsAt(c->start);
    right.fadeIn = 0.0;
    right.notes.clear();
    right.controls = cutLanes(c->controls, at);
    // Notes go to the part they start in
    std::vector<Note> left;
    for (const Note &n : c->notes) {
        if (n.start < at) {
            left.push_back(n);
        } else {
            Note moved = n;
            moved.start -= at;
            right.notes.push_back(moved);
        }
    }
    c->notes = std::move(left);
    c->length = at;
    c->fadeOut = 0.0;
    auto &clips = tracks[static_cast<size_t>(selectedTrack)].clips;
    clips.insert(clips.begin() + selectedClip + 1, std::move(right));
    return true;
}

void Project::trimStart(Clip &c, double beat, double minimum) const
{
    const double end = c.start + c.length;
    double start = std::min(beat, end - minimum);
    start = std::max(0.0, start);
    // An audio clip can't start before its file does
    if (c.hasAudio())
        start = std::max(start, timing.beatAt(timing.secondsAt(c.start) - c.offset));
    const double moved = start - c.start;
    c.offset = std::max(0.0, c.offset + timing.secondsAt(start) - timing.secondsAt(c.start));
    // Controller curves keep their place: what is before the new start goes, its value stays
    for (ControlLane &lane : c.controls) {
        const float value = lane.valueAt(moved);
        std::vector<ControlPoint> kept;
        if (moved > 0.0)
            kept.push_back({0.0, value});
        for (const ControlPoint &p : lane.points) {
            if (p.beat - moved >= (moved > 0.0 ? 1e-9 : -1e-9))
                kept.push_back({p.beat - moved, p.value});
        }
        lane.points = std::move(kept);
    }
    for (Note &n : c.notes)
        n.start -= moved;
    // Notes now before the clip's start fall out of it
    c.notes.erase(std::remove_if(c.notes.begin(), c.notes.end(), [](const Note &n) { return n.start < -1e-9; }),
                  c.notes.end());
    c.start = start;
    c.length = end - start;
}

void Project::deleteClip()
{
    if (!clip())
        return;
    auto &clips = tracks[static_cast<size_t>(selectedTrack)].clips;
    clips.erase(clips.begin() + selectedClip);
    selectedClip = -1;
}

void Project::duplicateClip()
{
    const Clip *c = clip();
    if (!c)
        return;
    Clip copy = *c;
    copy.start = std::min(c->start + c->length, lengthBeats() - c->length);
    auto &clips = tracks[static_cast<size_t>(selectedTrack)].clips;
    clips.push_back(copy);
    selectedClip = static_cast<int>(clips.size()) - 1;
}

namespace {

// Fingerpicked arpeggios over A minor, F, C and G, one chord a bar, in eighth notes
std::vector<Note> fingerpicking()
{
    const int chords[4][5] = {
        {45, 52, 57, 60, 64}, // Am
        {41, 48, 53, 57, 60}, // F
        {48, 52, 55, 60, 64}, // C
        {43, 50, 55, 59, 62}, // G
    };
    const int pattern[8] = {0, 1, 2, 3, 4, 3, 2, 1};
    std::vector<Note> notes;
    for (int bar = 0; bar < 8; ++bar) {
        const int *chord = chords[bar % 4];
        for (int step = 0; step < 8; ++step) {
            const bool root = step == 0;
            notes.push_back({bar * 4.0 + step * 0.5, root ? 4.0 : 1.0, chord[pattern[step]], root ? 0.85f : 0.55f + 0.05f * (step % 2)});
        }
    }
    return notes;
}

// The chords' roots, low, one to a bar
std::vector<Note> bassline()
{
    const int roots[4] = {33, 29, 36, 31}; // A1 F1 C2 G1
    std::vector<Note> notes;
    for (int bar = 0; bar < 8; ++bar) {
        notes.push_back({bar * 4.0, 2.0, roots[bar % 4], 0.9f});
        notes.push_back({bar * 4.0 + 2.5, 1.5, roots[bar % 4] + 12, 0.6f});
    }
    return notes;
}

// A short tune over the progression, twice
std::vector<Note> melody()
{
    const Note phrase[] = {
        {0.0, 1.0, 76, 0.8f}, {1.0, 1.0, 74, 0.7f}, {2.0, 2.0, 72, 0.75f},  // Am
        {4.0, 2.0, 69, 0.7f}, {6.0, 1.0, 72, 0.7f}, {7.0, 1.0, 74, 0.75f},  // F
        {8.0, 2.0, 76, 0.8f}, {10.0, 1.0, 74, 0.7f}, {11.0, 1.0, 72, 0.7f}, // C
        {12.0, 2.0, 74, 0.75f}, {14.0, 2.0, 71, 0.7f},                      // G
    };
    std::vector<Note> notes;
    for (int repeat = 0; repeat < 2; ++repeat) {
        for (Note n : phrase) {
            n.start += repeat * 16.0;
            notes.push_back(n);
        }
    }
    return notes;
}

}

std::vector<uint64_t> trackShape(const Project &project)
{
    std::vector<uint64_t> shape;
    shape.reserve(project.tracks.size());
    for (const Track &t : project.tracks)
        shape.push_back((static_cast<uint64_t>(t.id) << 2) | static_cast<uint64_t>(t.kind));
    return shape;
}

Project demoProject()
{
    Project p;
    p.name = "First session";
    p.exportPresets = defaultExportPresets();
    auto add = [&p](const char *name, TrackKind kind, float gain, float pan, std::vector<Clip> clips,
                    const char *instrument = "") {
        Track t;
        t.name = name;
        t.kind = kind;
        t.gain = gain;
        t.pan = pan;
        t.clips = std::move(clips);
        if (*instrument)
            t.instrument = audio::SamplerDevice::setting(instrument);
        p.tracks.push_back(t);
    };
    const char *guitar = "Spanish classical guitar";
    add("Guitar", TrackKind::Instrument, 0.8f, 0.15f,
        {{"Fingerpicking", 0.0, 32.0, fingerpicking()}, {"Fingerpicking", 32.0, 32.0, fingerpicking()},
         {"Fingerpicking", 64.0, 32.0, fingerpicking()}},
        guitar);
    add("Bass", TrackKind::Instrument, 0.7f, -0.1f,
        {{"Roots", 32.0, 32.0, bassline()}, {"Roots", 64.0, 32.0, bassline()}}, guitar);
    add("Lead", TrackKind::Instrument, 0.6f, -0.3f, {{"Tune", 64.0, 32.0, melody()}}, guitar);
    p.tracks.back().effects.push_back(audio::effectSetting(audio::EffectType::Reverb)); // a little room on the tune
    add("Drums", TrackKind::Audio, 0.9f, 0.0f, {{"Groove", 32.0, 64.0, {}}});
    add("Vocals", TrackKind::Audio, 1.0f, 0.0f, {{"Verse", 32.0, 16.0, {}}, {"Chorus", 64.0, 16.0, {}}});
    add("Ambience", TrackKind::Audio, 0.4f, 0.5f, {{"Room tone", 0.0, 96.0, {}}});
    p.bars = 32;
    p.select(0);
    ObjectId next = 1;
    p.normalizeIds(next); // made with its ids, as a project read from a file is
    return p;
}

const char *kindName(TrackKind kind)
{
    switch (kind) {
    case TrackKind::Audio:
        return "Audio";
    case TrackKind::Instrument:
        return "Instrument";
    case TrackKind::Return:
        return "Return";
    case TrackKind::Group:
        return "Group";
    }
    return "";
}

Project newProject(const audio::DeviceSetting &instrument, const NewProjectOptions &options)
{
    Project p;
    p.exportPresets = defaultExportPresets();
    p.name = options.name.empty() ? "Untitled" : options.name;
    p.timing = audio::TempoMap(options.tempo, options.beatsPerBar, options.beatUnit);
    p.bars = std::clamp(options.bars, 1, 9999);
    if (options.instrumentTrack) {
        p.addTrack(TrackKind::Instrument);
        p.tracks.back().instrument = instrument;
        p.tracks.back().clips.push_back({"Clip 1", 0.0, p.timing.barStart(4)});
    }
    if (options.audioTrack)
        p.addTrack(TrackKind::Audio);
    p.select(p.tracks.empty() ? -1 : 0, options.instrumentTrack ? 0 : -1);
    ObjectId next = 1;
    p.normalizeIds(next); // made with its ids, as a project read from a file is
    return p;
}

}
