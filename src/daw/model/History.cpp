#include "daw/model/History.h"

#include "daw/model/ProjectStore.h"

#include <algorithm>
#include <set>
#include <tuple>
#include <utility>

namespace daw {

namespace {

bool sameNote(const Note &a, const Note &b)
{
    return a.start == b.start && a.length == b.length && a.key == b.key && a.velocity == b.velocity &&
           a.muted == b.muted;
}

bool sameLanes(const std::vector<ControlLane> &a, const std::vector<ControlLane> &b)
{
    if (a.size() != b.size())
        return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (a[i].controller != b[i].controller || a[i].points.size() != b[i].points.size())
            return false;
        for (size_t p = 0; p < a[i].points.size(); ++p) {
            if (a[i].points[p].beat != b[i].points[p].beat || a[i].points[p].value != b[i].points[p].value)
                return false;
        }
    }
    return true;
}

bool sameClip(const Clip &a, const Clip &b)
{
    if (!sameLanes(a.controls, b.controls))
        return false;
    if (a.name != b.name || a.start != b.start || a.length != b.length || a.audioFile != b.audioFile ||
        a.offset != b.offset || a.gain != b.gain || a.fadeIn != b.fadeIn || a.fadeOut != b.fadeOut ||
        a.take != b.take || a.notes.size() != b.notes.size())
        return false;
    for (size_t i = 0; i < a.notes.size(); ++i) {
        if (!sameNote(a.notes[i], b.notes[i]))
            return false;
    }
    return true;
}

// Take lanes: names, takes and the takes' ids (the comp's sections name them, so they are content here)
bool sameTakeLanes(const std::vector<TakeLane> &a, const std::vector<TakeLane> &b)
{
    if (a.size() != b.size())
        return false;
    for (size_t l = 0; l < a.size(); ++l) {
        if (a[l].name != b[l].name || a[l].clips.size() != b[l].clips.size())
            return false;
        for (size_t i = 0; i < a[l].clips.size(); ++i) {
            if (a[l].clips[i].id != b[l].clips[i].id || !sameClip(a[l].clips[i], b[l].clips[i]))
                return false;
        }
    }
    return true;
}

// The comp's shape: each section's take and range (ordinary clips as no take)
std::vector<std::tuple<ObjectId, double, double>> compShape(const Track &t)
{
    std::vector<std::tuple<ObjectId, double, double>> shape;
    for (const Clip &c : t.clips)
        shape.emplace_back(c.take, c.start, c.length);
    return shape;
}

// The same clip, its notes' selection included: safe to share between snapshots
bool identicalClip(const Clip &a, const Clip &b)
{
    if (!sameClip(a, b))
        return false;
    for (size_t i = 0; i < a.notes.size(); ++i) {
        if (a.notes[i].selected != b.notes[i].selected)
            return false;
    }
    return true;
}

// Everything about a track but its clips that the history compares
bool sameTrackFields(const Track &a, const Track &b)
{
    return a.name == b.name && a.kind == b.kind && a.gain == b.gain && a.pan == b.pan && a.mute == b.mute &&
           a.solo == b.solo && a.instrument == b.instrument && a.inputChannel == b.inputChannel &&
           a.inputStereo == b.inputStereo && a.effects == b.effects && a.automation == b.automation &&
           a.output == b.output && a.sends == b.sends && sameTakeLanes(a.takeLanes, b.takeLanes);
}

bool sameTrack(const Track &a, const Track &b)
{
    if (!sameTrackFields(a, b) || a.clips.size() != b.clips.size())
        return false;
    for (size_t i = 0; i < a.clips.size(); ++i) {
        if (!sameClip(a.clips[i], b.clips[i]))
            return false;
    }
    return true;
}

// What changed between two clips' notes, when only one thing did
std::string describeNotes(const std::vector<Note> &a, const std::vector<Note> &b)
{
    if (b.size() > a.size())
        return b.size() - a.size() == 1 ? "Add note" : "Add notes";
    if (b.size() < a.size())
        return a.size() - b.size() == 1 ? "Delete note" : "Delete notes";
    bool keys = false, starts = false, lengths = false, velocities = false, mutes = false;
    for (size_t i = 0; i < a.size(); ++i) {
        keys = keys || a[i].key != b[i].key;
        starts = starts || a[i].start != b[i].start;
        lengths = lengths || a[i].length != b[i].length;
        velocities = velocities || a[i].velocity != b[i].velocity;
        mutes = mutes || a[i].muted != b[i].muted;
    }
    const int kinds = keys + starts + lengths + velocities + mutes;
    if (kinds != 1)
        return kinds == 2 && keys && starts ? "Move notes" : "Edit notes";
    if (keys)
        return "Transpose notes";
    if (starts)
        return "Move notes";
    if (lengths)
        return "Change note lengths";
    if (mutes)
        return "Mute notes";
    return "Change velocity";
}

// What changed between two effect chains
std::string describeEffects(const std::vector<audio::DeviceSetting> &a, const std::vector<audio::DeviceSetting> &b,
                            const char *where)
{
    const std::string place = std::string(" (") + where + ")";
    if (b.size() > a.size())
        return "Add effect" + place;
    if (b.size() < a.size())
        return "Remove effect" + place;
    for (size_t i = 0; i < a.size(); ++i) {
        if (a[i].kind != b[i].kind || a[i].id != b[i].id)
            return "Reorder effects" + place;
    }
    for (size_t i = 0; i < a.size(); ++i) {
        const std::string &name = b[i].name;
        if (a[i].bypass != b[i].bypass)
            return std::string(b[i].bypass ? "Bypass " : "Enable ") + name;
        if (a[i].sidechain != b[i].sidechain)
            return "Sidechain";
        if (a[i].values != b[i].values)
            return name + " settings";
        if (a[i].automation != b[i].automation)
            return "Edit automation";
    }
    return "Edit effects";
}

}

bool sameContent(const Project &a, const Project &b)
{
    if (a.name != b.name || a.timing != b.timing || a.markers != b.markers || a.bars != b.bars ||
        a.masterEffects != b.masterEffects || a.masterVolume != b.masterVolume ||
        a.masterAutomation != b.masterAutomation || a.exportPresets != b.exportPresets ||
        a.exportMetadata != b.exportMetadata || a.tracks.size() != b.tracks.size())
        return false;
    for (size_t i = 0; i < a.tracks.size(); ++i) {
        if (!sameTrack(a.tracks[i], b.tracks[i]))
            return false;
    }
    return true;
}

namespace {

// The device fields writeDevice writes: its state only when there is one
bool sameSavedDevice(const audio::DeviceSetting &a, const audio::DeviceSetting &b)
{
    if (a.kind != b.kind || a.id != b.id || a.name != b.name || a.bypass != b.bypass || a.sidechain != b.sidechain)
        return false;
    const bool stateA = a.state && !a.state->empty();
    const bool stateB = b.state && !b.state->empty();
    if (stateA != stateB || (stateA && *a.state != *b.state))
        return false;
    return a.values == b.values && a.automation == b.automation;
}

// A clip as writeProject writes it: an audio track's clips keep their audio, an instrument track's
// their controller lanes and notes
bool sameSavedClip(const Clip &a, const Clip &b, bool instrument)
{
    if (a.name != b.name || a.start != b.start || a.length != b.length)
        return false;
    if (!instrument) {
        if (a.hasAudio() != b.hasAudio())
            return false;
        return !a.hasAudio() || (a.audioFile == b.audioFile && a.offset == b.offset && a.gain == b.gain &&
                                 a.fadeIn == b.fadeIn && a.fadeOut == b.fadeOut);
    }
    if (a.controls.size() != b.controls.size() || a.notes.size() != b.notes.size())
        return false;
    for (size_t i = 0; i < a.controls.size(); ++i) {
        const ControlLane &x = a.controls[i];
        const ControlLane &y = b.controls[i];
        if (x.controller != y.controller || x.points.size() != y.points.size())
            return false;
        for (size_t p = 0; p < x.points.size(); ++p) {
            if (x.points[p].beat != y.points[p].beat || x.points[p].value != y.points[p].value)
                return false;
        }
    }
    for (size_t i = 0; i < a.notes.size(); ++i) {
        const Note &x = a.notes[i];
        const Note &y = b.notes[i];
        if (x.start != y.start || x.length != y.length || x.key != y.key || x.velocity != y.velocity || x.muted != y.muted)
            return false;
    }
    return true;
}

bool sameSavedTrack(const Track &a, const Track &b)
{
    if (a.kind != b.kind || a.name != b.name || a.gain != b.gain || a.pan != b.pan || a.mute != b.mute ||
        a.solo != b.solo || a.armed != b.armed || a.folded != b.folded || a.effects.size() != b.effects.size() ||
        a.clips.size() != b.clips.size() || a.automation != b.automation || a.output != b.output ||
        a.sends != b.sends)
        return false;
    const bool instrument = a.kind == TrackKind::Instrument;
    if (!instrument && (a.inputChannel != b.inputChannel || a.inputStereo != b.inputStereo || a.monitor != b.monitor))
        return false;
    if (instrument && (a.hasInstrument() != b.hasInstrument() ||
                       (a.hasInstrument() && !sameSavedDevice(a.instrument, b.instrument))))
        return false;
    for (size_t i = 0; i < a.effects.size(); ++i) {
        if (!sameSavedDevice(a.effects[i], b.effects[i]))
            return false;
    }
    for (size_t i = 0; i < a.clips.size(); ++i) {
        if (!sameSavedClip(a.clips[i], b.clips[i], instrument) || a.clips[i].take != b.clips[i].take)
            return false;
    }
    if (a.takeLanes.size() != b.takeLanes.size())
        return false;
    for (size_t l = 0; l < a.takeLanes.size(); ++l) {
        const TakeLane &x = a.takeLanes[l];
        const TakeLane &y = b.takeLanes[l];
        if (x.name != y.name || x.clips.size() != y.clips.size())
            return false;
        for (size_t i = 0; i < x.clips.size(); ++i) {
            if (x.clips[i].id != y.clips[i].id || !sameSavedClip(x.clips[i], y.clips[i], instrument))
                return false;
        }
    }
    return true;
}

}

bool sameSaved(const Project &a, const Project &b)
{
    if (a.name != b.name || a.timing != b.timing || a.markers != b.markers || a.bars != b.bars ||
        a.hasLoop() != b.hasLoop() || a.masterEffects.size() != b.masterEffects.size() || a.masterVolume != b.masterVolume ||
        a.masterAutomation != b.masterAutomation ||
        a.exportPresets != b.exportPresets || a.exportMetadata != b.exportMetadata || a.tracks.size() != b.tracks.size())
        return false;
    if (a.hasLoop() && (a.loopStart != b.loopStart || a.loopEnd != b.loopEnd || a.looping != b.looping))
        return false;
    for (size_t i = 0; i < a.masterEffects.size(); ++i) {
        if (!sameSavedDevice(a.masterEffects[i], b.masterEffects[i]))
            return false;
    }
    for (size_t i = 0; i < a.tracks.size(); ++i) {
        if (!sameSavedTrack(a.tracks[i], b.tracks[i]))
            return false;
    }
    return true;
}

bool sameSelection(const Project &a, const Project &b)
{
    if (a.selectedTrack != b.selectedTrack || a.selectedClip != b.selectedClip || a.tracks.size() != b.tracks.size())
        return false;
    for (size_t t = 0; t < a.tracks.size(); ++t) {
        const auto &ca = a.tracks[t].clips;
        const auto &cb = b.tracks[t].clips;
        if (ca.size() != cb.size())
            return false;
        for (size_t c = 0; c < ca.size(); ++c) {
            if (ca[c].notes.size() != cb[c].notes.size())
                return false;
            for (size_t n = 0; n < ca[c].notes.size(); ++n) {
                if (ca[c].notes[n].selected != cb[c].notes[n].selected)
                    return false;
            }
        }
    }
    return true;
}

std::string describeChange(const Project &a, const Project &b)
{
    if (b.tracks.size() > a.tracks.size())
        return b.tracks.size() - a.tracks.size() == 1 ? "Add track" : "Add tracks";
    if (b.tracks.size() < a.tracks.size())
        return a.tracks.size() - b.tracks.size() == 1 ? "Delete track" : "Delete tracks";
    for (size_t i = 0; i < a.tracks.size(); ++i) {
        if (a.tracks[i].id != b.tracks[i].id)
            return "Move track";
    }
    if (a.timing.points() != b.timing.points())
        return "Change tempo";
    if (a.timing.signatures() != b.timing.signatures())
        return "Change time signature";
    if (a.markers != b.markers) {
        if (b.markers.size() > a.markers.size())
            return "Add marker";
        if (b.markers.size() < a.markers.size())
            return "Delete marker";
        for (size_t i = 0; i < a.markers.size(); ++i) {
            if (a.markers[i].name != b.markers[i].name)
                return "Rename marker";
        }
        return "Move marker";
    }
    if (a.name != b.name || a.bars != b.bars)
        return "Project settings";

    if (a.masterEffects != b.masterEffects)
        return describeEffects(a.masterEffects, b.masterEffects, "master");
    if (a.masterVolume != b.masterVolume)
        return "Master volume";
    if (a.masterAutomation != b.masterAutomation)
        return "Edit automation";
    if (a.exportPresets != b.exportPresets)
        return "Edit export preset";
    if (a.exportMetadata != b.exportMetadata)
        return "Edit export details";

    // Takes recorded, removed or comped (before the clips: comping cuts and joins them)
    for (size_t i = 0; i < a.tracks.size(); ++i) {
        const Track &ta = a.tracks[i];
        const Track &tb = b.tracks[i];
        if (!sameTakeLanes(ta.takeLanes, tb.takeLanes)) {
            if (tb.takeLanes.size() > ta.takeLanes.size())
                return "Record takes";
            if (tb.takeLanes.empty())
                return "Flatten comp";
            if (tb.takeLanes.size() < ta.takeLanes.size())
                return "Delete take lane";
            return "Edit takes";
        }
        if (!tb.takeLanes.empty() && compShape(ta) != compShape(tb))
            return "Comp";
    }

    // Clips appearing on one track and going from another moved between them
    size_t added = 0, removed = 0;
    for (size_t i = 0; i < a.tracks.size(); ++i) {
        const size_t before = a.tracks[i].clips.size(), after = b.tracks[i].clips.size();
        added += after > before ? after - before : 0;
        removed += before > after ? before - after : 0;
    }
    if (added > 0 && removed > 0)
        return "Move clip to another track";
    if (added > 0)
        return added == 1 ? "Add clip" : "Add clips";
    if (removed > 0)
        return removed == 1 ? "Delete clip" : "Delete clips";

    for (size_t i = 0; i < a.tracks.size(); ++i) {
        const Track &ta = a.tracks[i];
        const Track &tb = b.tracks[i];
        if (ta.kind != tb.kind)
            return "Change tracks";
        if (ta.name != tb.name)
            return "Rename track";
        if (ta.instrument.kind != tb.instrument.kind || ta.instrument.id != tb.instrument.id)
            return tb.hasInstrument() ? "Change instrument" : "Remove instrument";
        if (ta.instrument.automation != tb.instrument.automation)
            return "Edit automation";
        if (ta.instrument != tb.instrument)
            return tb.instrument.name + " settings";
        if (ta.gain != tb.gain)
            return "Track volume";
        if (ta.pan != tb.pan)
            return "Track pan";
        if (ta.mute != tb.mute)
            return tb.mute ? "Mute track" : "Unmute track";
        if (ta.solo != tb.solo)
            return tb.solo ? "Solo track" : "Unsolo track";
        if (ta.inputChannel != tb.inputChannel || ta.inputStereo != tb.inputStereo)
            return "Track input";
        if (ta.output != tb.output)
            return "Route track";
        if (ta.sends.size() != tb.sends.size())
            return tb.sends.size() > ta.sends.size() ? "Add send" : "Remove send";
        if (ta.sends != tb.sends)
            return "Change send";
        if (ta.automation != tb.automation)
            return "Edit automation";
        if (ta.effects != tb.effects)
            return describeEffects(ta.effects, tb.effects, "track");
        for (size_t c = 0; c < ta.clips.size(); ++c) {
            const Clip &ca = ta.clips[c];
            const Clip &cb = tb.clips[c];
            if (sameClip(ca, cb))
                continue;
            if (ca.name != cb.name)
                return "Rename clip";
            if (ca.audioFile != cb.audioFile)
                return "Change clip audio";
            const bool notesChanged = !(ca.notes.size() == cb.notes.size() && [&] {
                for (size_t n = 0; n < ca.notes.size(); ++n) {
                    if (!sameNote(ca.notes[n], cb.notes[n]))
                        return false;
                }
                return true;
            }());
            // Trimming a clip's front moves its notes against it, so the edges come first
            if (ca.length != cb.length || ca.offset != cb.offset)
                return "Trim clip";
            if (ca.start != cb.start)
                return "Move clip";
            if (notesChanged)
                return describeNotes(ca.notes, cb.notes);
            if (!sameLanes(ca.controls, cb.controls)) {
                // The lane that changed, by name
                for (const ControlLane &lane : cb.controls) {
                    const auto was = std::find_if(ca.controls.begin(), ca.controls.end(),
                                                  [&lane](const ControlLane &l) { return l.controller == lane.controller; });
                    if (was == ca.controls.end() || !sameLanes({*was}, {lane}))
                        return "Edit " + controllerName(lane.controller);
                }
                return "Clear controller";
            }
            if (ca.gain != cb.gain)
                return "Clip gain";
            return "Clip fades";
        }
    }
    return "Edit";
}

namespace {

// Track's fields are listed by hand here (shellOf, sameTrackFields) and in ProjectStore::restore, so
// a field added to Track would quietly drop out of undo. This stops the build when Track's size
// changes (on the toolchain the project builds with): add the field to those three, then update the size.
#if defined(__GLIBCXX__) && defined(__x86_64__)
static_assert(sizeof(Track) == 344, "Track changed: add the new field to shellOf, sameTrackFields and "
                                     "ProjectStore::restore, then update this size");
#endif

// A track without its clips. Keep in step with Track: a field left out here is lost from undo.
Track shellOf(const Track &t)
{
    Track s;
    s.id = t.id;
    s.name = t.name;
    s.kind = t.kind;
    s.gain = t.gain;
    s.pan = t.pan;
    s.mute = t.mute;
    s.solo = t.solo;
    s.armed = t.armed;
    s.folded = t.folded;
    s.instrument = t.instrument;
    s.inputChannel = t.inputChannel;
    s.inputStereo = t.inputStereo;
    s.monitor = t.monitor;
    s.effects = t.effects;
    s.automation = t.automation;
    s.output = t.output;
    s.sends = t.sends;
    s.takeLanes = t.takeLanes;
    return s;
}

// The project's own fields, with no tracks
Project rootOf(const Project &p)
{
    Project r;
    r.name = p.name;
    r.timing = p.timing;
    r.bars = p.bars;
    r.loopStart = p.loopStart;
    r.loopEnd = p.loopEnd;
    r.looping = p.looping;
    r.markers = p.markers;
    r.masterEffects = p.masterEffects;
    r.masterVolume = p.masterVolume;
    r.masterAutomation = p.masterAutomation;
    r.exportPresets = p.exportPresets;
    r.exportMetadata = p.exportMetadata;
    r.selectedTrack = p.selectedTrack;
    r.selectedClip = p.selectedClip;
    return r;
}

// The same shell, ids, arming, folding and monitoring included: safe to share between snapshots
bool identicalShell(const Track &a, const Track &b)
{
    return sameTrackFields(a, b) && a.id == b.id && a.armed == b.armed && a.folded == b.folded && a.monitor == b.monitor;
}

// Whether the project's own fields (all but the tracks and the selection) differ from the shell's
bool fieldsDiffer(const Project &project, const Project &shell, bool master)
{
    return project.name != shell.name || project.timing != shell.timing || project.markers != shell.markers ||
           project.bars != shell.bars || project.exportPresets != shell.exportPresets ||
           project.exportMetadata != shell.exportMetadata || (master && (project.masterEffects != shell.masterEffects || project.masterVolume != shell.masterVolume ||
                                                    project.masterAutomation != shell.masterAutomation));
}

// The fields editMix covers, less arming, folding and monitoring (which undo leaves as they are)
bool sameMixFields(const Track &a, const Track &b)
{
    return a.name == b.name && a.gain == b.gain && a.pan == b.pan && a.mute == b.mute && a.solo == b.solo &&
           a.inputChannel == b.inputChannel && a.inputStereo == b.inputStereo && a.output == b.output &&
           a.sends == b.sends;
}

}

bool History::sameEntry(const TrackEntry &entry, const Track &track)
{
    if (!entry.shell || !sameTrackFields(*entry.shell, track) || entry.clips.size() != track.clips.size())
        return false;
    for (size_t i = 0; i < track.clips.size(); ++i) {
        if (!sameClip(*entry.clips[i], track.clips[i]))
            return false;
    }
    return true;
}

void History::syncEntry(TrackEntry &entry, const Track &track)
{
    if (!entry.shell || !identicalShell(*entry.shell, track))
        entry.shell = std::make_shared<const Track>(shellOf(track));
    entry.clips.resize(track.clips.size());
    for (size_t c = 0; c < track.clips.size(); ++c) {
        if (!entry.clips[c] || !identicalClip(*entry.clips[c], track.clips[c]))
            entry.clips[c] = std::make_shared<const Clip>(track.clips[c]);
    }
}

Track History::unpackTrack(const TrackEntry &entry)
{
    Track track = *entry.shell;
    track.clips.reserve(entry.clips.size());
    for (const auto &clip : entry.clips)
        track.clips.push_back(*clip);
    return track;
}

Project History::unpack(const Snapshot &snapshot)
{
    Project project = snapshot.shell;
    project.tracks.reserve(snapshot.tracks.size());
    for (const TrackEntry &entry : snapshot.tracks)
        project.tracks.push_back(unpackTrack(entry));
    return project;
}

HistoryRestore History::makePlan(const Snapshot &from, const Snapshot &to, const std::string &label)
{
    HistoryRestore plan;
    plan.label = label;
    // The tracks coming, going or moving (or changing kind) can only be applied by replacing the project
    bool structural = from.tracks.size() != to.tracks.size();
    for (size_t i = 0; i < from.tracks.size() && !structural; ++i)
        structural = from.tracks[i].shell->id != to.tracks[i].shell->id || from.tracks[i].shell->kind != to.tracks[i].shell->kind;
    plan.timing = from.shell.timing != to.shell.timing;
    if (structural) {
        plan.structural = true;
        plan.timing = true;
        plan.project = unpack(to);
        return plan;
    }
    plan.project = rootOf(to.shell);
    plan.settings = from.shell.name != to.shell.name || from.shell.bars != to.shell.bars;
    plan.markers = from.shell.markers != to.shell.markers;
    plan.presets = from.shell.exportPresets != to.shell.exportPresets;
    plan.metadata = from.shell.exportMetadata != to.shell.exportMetadata;
    plan.master = from.shell.masterEffects != to.shell.masterEffects || from.shell.masterVolume != to.shell.masterVolume ||
                   from.shell.masterAutomation != to.shell.masterAutomation;
    plan.selection = from.shell.selectedTrack != to.shell.selectedTrack || from.shell.selectedClip != to.shell.selectedClip;
    // Only the tracks whose parts differ, found by pointer and confirmed by comparing where they do
    for (size_t i = 0; i < from.tracks.size(); ++i) {
        const TrackEntry &a = from.tracks[i];
        const TrackEntry &b = to.tracks[i];
        bool content = false, mix = false;
        if (a.shell != b.shell) {
            mix = !sameMixFields(*a.shell, *b.shell);
            content = a.shell->instrument != b.shell->instrument || a.shell->effects != b.shell->effects ||
                      a.shell->automation != b.shell->automation || !sameTakeLanes(a.shell->takeLanes, b.shell->takeLanes);
        }
        if (a.clips.size() != b.clips.size())
            content = true;
        for (size_t c = 0; c < a.clips.size() && !content; ++c)
            content = a.clips[c] != b.clips[c] && !identicalClip(*a.clips[c], *b.clips[c]);
        if (content || mix)
            plan.tracks.push_back({i, unpackTrack(b), content, mix});
    }
    return plan;
}

size_t History::storedClips() const
{
    std::set<const Clip *> clips;
    for (const auto *steps : {&undo_, &redo_}) {
        for (const Step &step : *steps) {
            for (const TrackEntry &track : step.snapshot.tracks) {
                for (const auto &clip : track.clips)
                    clips.insert(clip.get());
            }
        }
    }
    return clips.size();
}

size_t History::storedBytes() const
{
    std::set<const Clip *> clips;
    std::set<const Track *> shells;
    size_t bytes = 0;
    for (const auto *steps : {&undo_, &redo_}) {
        for (const Step &step : *steps) {
            bytes += sizeof(Step) + sizeof(Project) + step.label.size();
            for (const TrackEntry &track : step.snapshot.tracks) {
                bytes += sizeof(TrackEntry) + track.clips.size() * sizeof(std::shared_ptr<const Clip>);
                if (shells.insert(track.shell.get()).second)
                    bytes += sizeof(Track) + track.shell->effects.size() * sizeof(audio::DeviceSetting);
                for (const auto &clip : track.clips) {
                    if (clips.insert(clip.get()).second)
                        bytes += sizeof(Clip) + clip->notes.size() * sizeof(Note);
                }
            }
        }
    }
    return bytes;
}

bool History::matches(const Project &project) const
{
    if (fieldsDiffer(project, state_.shell, true) || project.tracks.size() != state_.tracks.size())
        return false;
    for (size_t i = 0; i < project.tracks.size(); ++i) {
        if (!sameEntry(state_.tracks[i], project.tracks[i]))
            return false;
    }
    return true;
}

const Project &History::current() const
{
    currentCache_ = unpack(state_);
    return currentCache_;
}

void History::sync(const Project &project, const std::vector<char> &moved, bool structure, bool fields, bool master)
{
    Project &shell = state_.shell;
    if (fields) {
        shell.name = project.name;
        shell.timing = project.timing;
        shell.markers = project.markers;
        shell.bars = project.bars;
        shell.loopStart = project.loopStart;
        shell.loopEnd = project.loopEnd;
        shell.looping = project.looping;
        shell.exportPresets = project.exportPresets;
        shell.exportMetadata = project.exportMetadata;
    }
    if (master) {
        shell.masterEffects = project.masterEffects;
        shell.masterVolume = project.masterVolume;
        shell.masterAutomation = project.masterAutomation;
    }
    shell.selectedTrack = project.selectedTrack;
    shell.selectedClip = project.selectedClip;
    if (structure) {
        // Tracks may have come, gone or moved: each takes the old entry with its id, so what didn't
        // change is still shared
        std::vector<TrackEntry> old = std::move(state_.tracks);
        state_.tracks.assign(project.tracks.size(), TrackEntry());
        for (size_t i = 0; i < project.tracks.size(); ++i) {
            const Track &track = project.tracks[i];
            const TrackEntry *from = i < old.size() ? &old[i] : nullptr;
            if (track.id != kNoId) {
                for (const TrackEntry &entry : old) {
                    if (entry.shell && entry.shell->id == track.id) {
                        from = &entry;
                        break;
                    }
                }
            }
            if (from)
                state_.tracks[i] = *from;
            syncEntry(state_.tracks[i], track);
        }
        return;
    }
    for (size_t i = 0; i < project.tracks.size(); ++i) {
        if (moved[i])
            syncEntry(state_.tracks[i], project.tracks[i]);
    }
}

void History::markSeen(const ProjectStore *store)
{
    known_ = store != nullptr;
    if (!store)
        return;
    structureSeen_ = store->structureRevision();
    revisionSeen_ = store->revision();
    masterSeen_ = store->masterRevision();
    const size_t count = state_.tracks.size();
    trackSeen_.resize(count);
    mixSeen_.resize(count);
    for (size_t i = 0; i < count; ++i) {
        trackSeen_[i] = store->trackRevision(static_cast<int>(i));
        mixSeen_[i] = store->mixRevision(static_cast<int>(i));
    }
}

void History::adopt(const ProjectStore &store)
{
    pending_ = false;
    // Only when the store has the tracks the state has; otherwise the next look compares everything
    if (store.project().tracks.size() == state_.tracks.size())
        markSeen(&store);
    else
        known_ = false;
}

void History::reset(const Project &project)
{
    state_ = Snapshot();
    sync(project, std::vector<char>(project.tracks.size(), 1), true, true, true);
    undo_.clear();
    redo_.clear();
    lastTime_ = -1e9;
    known_ = false;
    pending_ = false;
}

History::Compare History::compare(const Project &project, const ProjectStore *store) const
{
    const size_t count = project.tracks.size();
    Compare c;
    c.moved.assign(count, 1);
    // What needs comparing: with revisions to go by, the tracks whose revisions moved since the last
    // state recorded; otherwise, or once the structure moved, all of them
    if (store && known_ && count == state_.tracks.size() && trackSeen_.size() == count &&
        store->structureRevision() == structureSeen_) {
        c.structure = false;
        c.fields = store->revision() != revisionSeen_;
        c.master = store->masterRevision() != masterSeen_;
        for (size_t i = 0; i < count; ++i) {
            c.moved[i] = store->trackRevision(static_cast<int>(i)) != trackSeen_[i] ||
                         store->mixRevision(static_cast<int>(i)) != mixSeen_[i];
        }
    }
    c.differs = (c.fields && fieldsDiffer(project, state_.shell, c.master)) || count != state_.tracks.size();
    for (size_t i = 0; i < count && !c.differs; ++i)
        c.differs = c.moved[i] && !sameEntry(state_.tracks[i], project.tracks[i]);
    return c;
}

bool History::look(const Project &project, const ProjectStore *store, bool settled, bool canMerge, double now)
{
    if (pending_ && !settled)
        return false; // a difference is already seen; nothing can change until the settled frame
    const Compare c = compare(project, store);
    if (c.differs) {
        if (!settled) {
            pending_ = true;
            return false;
        }
        std::string label;
        if (!c.structure) {
            // The same tracks in the same order: only the ones that moved can differ, so the label is
            // found from those alone (the rest would add nothing to it)
            Project before = rootOf(state_.shell);
            Project after = rootOf(project);
            for (size_t i = 0; i < c.moved.size(); ++i) {
                if (c.moved[i]) {
                    before.tracks.push_back(unpackTrack(state_.tracks[i]));
                    after.tracks.push_back(project.tracks[i]);
                }
            }
            label = describeChange(before, after);
        } else {
            label = describeChange(unpack(state_), project);
        }
        const bool merge = canMerge && !undo_.empty() && undo_.back().label == label && now - lastTime_ < 0.5;
        if (!merge) {
            undo_.push_back({state_, std::move(label)});
            if (undo_.size() > kMaxSteps)
                undo_.erase(undo_.begin());
        }
        redo_.clear();
        lastTime_ = now;
    }
    // The state recorded takes in the change, or, when only the selection moved, just that: remembered,
    // so undo puts it back as it was before the edit
    sync(project, c.moved, c.structure, c.fields, c.master);
    markSeen(store);
    pending_ = false;
    return c.differs;
}

bool History::update(const ProjectStore &store, bool settled, bool canMerge, double now)
{
    return look(store.project(), &store, settled, canMerge, now);
}

bool History::update(const Project &project, bool settled, bool canMerge, double now)
{
    return look(project, nullptr, settled, canMerge, now);
}

bool History::takeStep(bool forward, const Project &current, const ProjectStore *store, double now, HistoryRestore *plan)
{
    if (!forward) {
        look(current, store, true, false, now); // a change still waiting is the one to take back
        if (undo_.empty())
            return false;
    } else {
        if (redo_.empty())
            return false;
        const Compare c = compare(current, store);
        if (c.differs)
            return false; // an edit since the undo has replaced what could be redone
        // The step to undo this one is the project as it is now, its selection included
        sync(current, c.moved, c.structure, c.fields, c.master);
    }
    auto &from = forward ? redo_ : undo_;
    Step step = std::move(from.back());
    from.pop_back();
    (forward ? undo_ : redo_).push_back({state_, step.label});
    if (plan) {
        *plan = makePlan(state_, step.snapshot, step.label);
        plan->revision = store ? store->revision() : 0;
    }
    state_ = std::move(step.snapshot);
    known_ = false;
    pending_ = false;
    lastTime_ = -1e9;
    return true;
}

bool History::undo(const ProjectStore &store, double now, HistoryRestore &plan)
{
    return takeStep(false, store.project(), &store, now, &plan);
}

bool History::redo(const ProjectStore &store, HistoryRestore &plan)
{
    return takeStep(true, store.project(), &store, 0.0, &plan);
}

const Project *History::undo(const Project &current, double now)
{
    if (!takeStep(false, current, nullptr, now, nullptr))
        return nullptr;
    currentCache_ = unpack(state_);
    return &currentCache_;
}

const Project *History::redo(const Project &current)
{
    if (!takeStep(true, current, nullptr, 0.0, nullptr))
        return nullptr;
    currentCache_ = unpack(state_);
    return &currentCache_;
}

}
