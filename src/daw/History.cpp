#include "History.h"

#include <algorithm>
#include <set>
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
        a.notes.size() != b.notes.size())
        return false;
    for (size_t i = 0; i < a.notes.size(); ++i) {
        if (!sameNote(a.notes[i], b.notes[i]))
            return false;
    }
    return true;
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

bool sameTrack(const Track &a, const Track &b)
{
    if (a.name != b.name || a.kind != b.kind || a.gain != b.gain || a.pan != b.pan || a.mute != b.mute ||
        a.solo != b.solo || a.instrument != b.instrument || a.inputChannel != b.inputChannel ||
        a.inputStereo != b.inputStereo || a.effects != b.effects || a.clips.size() != b.clips.size())
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
        if (a[i].values != b[i].values)
            return name + " settings";
    }
    return "Edit effects";
}

}

bool sameContent(const Project &a, const Project &b)
{
    if (a.name != b.name || a.tempo != b.tempo || a.beatsPerBar != b.beatsPerBar || a.bars != b.bars ||
        a.masterEffects != b.masterEffects || a.exportPresets != b.exportPresets ||
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
    if (a.kind != b.kind || a.id != b.id || a.name != b.name || a.bypass != b.bypass)
        return false;
    const bool stateA = a.state && !a.state->empty();
    const bool stateB = b.state && !b.state->empty();
    if (stateA != stateB || (stateA && *a.state != *b.state))
        return false;
    return a.values == b.values;
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
        a.solo != b.solo || a.armed != b.armed || a.effects.size() != b.effects.size() ||
        a.clips.size() != b.clips.size())
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
        if (!sameSavedClip(a.clips[i], b.clips[i], instrument))
            return false;
    }
    return true;
}

}

bool sameSaved(const Project &a, const Project &b)
{
    if (a.name != b.name || a.tempo != b.tempo || a.beatsPerBar != b.beatsPerBar || a.bars != b.bars ||
        a.hasLoop() != b.hasLoop() || a.masterEffects.size() != b.masterEffects.size() ||
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
    if (a.tempo != b.tempo)
        return "Change tempo";
    if (a.name != b.name || a.beatsPerBar != b.beatsPerBar || a.bars != b.bars)
        return "Project settings";

    if (a.masterEffects != b.masterEffects)
        return describeEffects(a.masterEffects, b.masterEffects, "master");
    if (a.exportPresets != b.exportPresets)
        return "Edit export preset";
    if (a.exportMetadata != b.exportMetadata)
        return "Edit export details";

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

History::Snapshot History::pack(Project project, const Snapshot *like)
{
    Snapshot s;
    s.clips.resize(project.tracks.size());
    for (size_t t = 0; t < project.tracks.size(); ++t) {
        std::vector<Clip> &clips = project.tracks[t].clips;
        for (size_t c = 0; c < clips.size(); ++c) {
            const std::shared_ptr<const Clip> *same = nullptr;
            if (like && t < like->clips.size() && c < like->clips[t].size() && identicalClip(*like->clips[t][c], clips[c]))
                same = &like->clips[t][c];
            s.clips[t].push_back(same ? *same : std::make_shared<const Clip>(std::move(clips[c])));
        }
        clips.clear();
        clips.shrink_to_fit();
    }
    s.shell = std::move(project);
    return s;
}

Project History::unpack(const Snapshot &snapshot)
{
    Project project = snapshot.shell;
    for (size_t t = 0; t < project.tracks.size() && t < snapshot.clips.size(); ++t) {
        project.tracks[t].clips.reserve(snapshot.clips[t].size());
        for (const auto &clip : snapshot.clips[t])
            project.tracks[t].clips.push_back(*clip);
    }
    return project;
}

size_t History::storedClips() const
{
    std::set<const Clip *> clips;
    for (const auto *steps : {&undo_, &redo_}) {
        for (const Step &step : *steps) {
            for (const auto &track : step.snapshot.clips) {
                for (const auto &clip : track)
                    clips.insert(clip.get());
            }
        }
    }
    return clips.size();
}

void History::reset(const Project &project)
{
    state_ = project;
    undo_.clear();
    redo_.clear();
    lastTime_ = -1e9;
}

bool History::update(const Project &project, bool settled, bool canMerge, double now)
{
    if (sameContent(project, state_)) {
        // Only the selection moved: remembered, so undo puts it back as it was before the edit
        if (!sameSelection(project, state_))
            state_ = project;
        return false;
    }
    if (!settled)
        return false;
    std::string label = describeChange(state_, project);
    const bool merge = canMerge && !undo_.empty() && undo_.back().label == label && now - lastTime_ < 0.5;
    if (!merge) {
        Snapshot packed = pack(std::move(state_), undo_.empty() ? nullptr : &undo_.back().snapshot);
        undo_.push_back({std::move(packed), std::move(label)});
        if (undo_.size() > kMaxSteps)
            undo_.erase(undo_.begin());
    }
    redo_.clear();
    state_ = project;
    lastTime_ = now;
    return true;
}

const Project *History::undo(const Project &current, double now)
{
    update(current, true, false, now); // a change still waiting is the one to take back
    if (undo_.empty())
        return nullptr;
    Step step = std::move(undo_.back());
    undo_.pop_back();
    Snapshot packed = pack(std::move(state_), &step.snapshot);
    redo_.push_back({std::move(packed), step.label});
    state_ = unpack(step.snapshot);
    lastTime_ = -1e9;
    return &state_;
}

const Project *History::redo(const Project &current)
{
    if (redo_.empty() || !sameContent(current, state_))
        return nullptr; // an edit since the undo has replaced what could be redone
    Step step = std::move(redo_.back());
    redo_.pop_back();
    Snapshot packed = pack(current, &step.snapshot);
    undo_.push_back({std::move(packed), step.label});
    state_ = unpack(step.snapshot);
    lastTime_ = -1e9;
    return &state_;
}

}
