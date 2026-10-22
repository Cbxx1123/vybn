#include "daw/model/ProjectStore.h"

#include <utility>

namespace daw {

namespace {

// A track's id followed by its clips' ids
std::vector<ObjectId> idsOf(const Track &t)
{
    std::vector<ObjectId> ids{t.id};
    for (const Clip &c : t.clips)
        ids.push_back(c.id);
    return ids;
}

}

ProjectStore::ProjectStore(Project project)
    : project_(std::move(project)), trackRevision_(project_.tracks.size(), 1), mixRevision_(project_.tracks.size(), 1)
{
    // The starting project gets its ids as part of revision 1, so nothing moves
    project_.normalizeIds(nextId_);
    idsDirty_ = false;
}

Project &ProjectStore::edit()
{
    settle();
    idsDirty_ = true; // tracks or clips may have been added or copied
    revision_ = bump();
    // Anything may change: the master's effects and the selection too
    masterRevision_ = bump();
    selectionRevision_ = bump();
    structureDirty_ = true; // the tracks are given fresh revisions at the next read or edit
    return project_;
}

Track &ProjectStore::editTrack(int track)
{
    settle();
    idsDirty_ = true; // clips may have been added or copied
    revision_ = bump();
    trackRevision_.at(static_cast<size_t>(track)) = bump();
    return project_.tracks.at(static_cast<size_t>(track));
}

Track &ProjectStore::editMix(int track)
{
    settle();
    revision_ = bump();
    mixRevision_.at(static_cast<size_t>(track)) = bump();
    return project_.tracks.at(static_cast<size_t>(track));
}

Clip &ProjectStore::editClip(int track, int clip)
{
    return editTrack(track).clips.at(static_cast<size_t>(clip));
}

std::vector<audio::DeviceSetting> &ProjectStore::editMasterEffects()
{
    settle();
    revision_ = bump();
    masterRevision_ = bump();
    return project_.masterEffects;
}

float &ProjectStore::editMasterVolume()
{
    settle();
    revision_ = bump();
    masterRevision_ = bump();
    return project_.masterVolume;
}

std::vector<audio::AutomationLane> &ProjectStore::editMasterAutomation()
{
    settle();
    revision_ = bump();
    masterRevision_ = bump();
    return project_.masterAutomation;
}

Project &ProjectStore::editSettings()
{
    settle();
    revision_ = bump();
    return project_;
}

std::vector<ExportPreset> &ProjectStore::editExportPresets()
{
    settle();
    revision_ = bump();
    return project_.exportPresets;
}

ExportMetadata &ProjectStore::editExportMetadata()
{
    settle();
    revision_ = bump();
    return project_.exportMetadata;
}

audio::DeviceSetting *ProjectStore::editDevice(const DevicePlace &place)
{
    // Looked up first, so a place with no device counts as no change
    if (!project_.device(place))
        return nullptr;
    if (place.master)
        editMasterEffects();
    else
        editTrack(place.track);
    return project_.device(place);
}

void ProjectStore::select(int track, int clip)
{
    settle();
    const int oldTrack = project_.selectedTrack;
    const int oldClip = project_.selectedClip;
    project_.select(track, clip);
    if (project_.selectedTrack != oldTrack || project_.selectedClip != oldClip)
        selectionRevision_ = bump();
}

void ProjectStore::setTiming(const audio::TempoMap &timing)
{
    settle();
    if (project_.timing == timing)
        return;
    project_.timing = timing;
    revision_ = bump();
}

void ProjectStore::setLoop(double start, double end, bool looping)
{
    settle();
    if (project_.loopStart == start && project_.loopEnd == end && project_.looping == looping)
        return;
    project_.loopStart = start;
    project_.loopEnd = end;
    project_.looping = looping;
    revision_ = bump();
}

void ProjectStore::toggleLoop()
{
    settle();
    project_.toggleLoop();
    revision_ = bump();
}

void ProjectStore::replace(Project project)
{
    project_ = std::move(project);
    revision_ = bump();
    masterRevision_ = bump();
    selectionRevision_ = bump();
    structureDirty_ = true;
    settle();
    // nextId_ is kept: an id given earlier in the session is never given again
    idsDirty_ = true;
    normalizeIds();
}

void ProjectStore::restore(HistoryRestore plan)
{
    if (plan.structural) {
        // Arming, folding and monitoring aren't part of the history: they stay as they are now, kept by track id
        Project state = std::move(plan.project);
        for (Track &t : state.tracks) {
            const Track *now = project_.findTrack(t.id);
            if (now && now->kind == t.kind) {
                t.armed = now->armed;
                t.folded = now->folded;
                t.monitor = now->monitor;
            }
        }
        // Nor is the loop
        state.loopStart = project_.loopStart;
        state.loopEnd = project_.loopEnd;
        state.looping = project_.looping;
        // The bridge matches the tracks by id in sync, so a kept track keeps its processor and plugins
        replace(std::move(state));
        return;
    }
    for (HistoryRestore::TrackChange &change : plan.tracks) {
        if (change.index >= project_.tracks.size())
            continue;
        const int index = static_cast<int>(change.index);
        if (change.content) {
            Track &t = editTrack(index);
            t.instrument = std::move(change.track.instrument);
            t.effects = std::move(change.track.effects);
            t.clips = std::move(change.track.clips);
            t.automation = std::move(change.track.automation);
            t.takeLanes = std::move(change.track.takeLanes);
        }
        if (change.mix) {
            Track &t = editMix(index);
            t.name = std::move(change.track.name);
            t.gain = change.track.gain;
            t.pan = change.track.pan;
            t.mute = change.track.mute;
            t.solo = change.track.solo;
            t.inputChannel = change.track.inputChannel;
            t.inputStereo = change.track.inputStereo;
            t.output = change.track.output;
            t.sends = std::move(change.track.sends);
        }
    }
    const Project &fields = plan.project;
    if (plan.master) {
        if (project_.masterEffects != fields.masterEffects)
            editMasterEffects() = fields.masterEffects;
        if (project_.masterVolume != fields.masterVolume)
            editMasterVolume() = fields.masterVolume;
        if (project_.masterAutomation != fields.masterAutomation)
            editMasterAutomation() = fields.masterAutomation;
    }
    if (plan.timing)
        setTiming(fields.timing);
    if (plan.settings) {
        Project &p = editSettings();
        p.name = fields.name;
        p.bars = fields.bars;
    }
    if (plan.presets)
        editExportPresets() = fields.exportPresets;
    if (plan.metadata)
        editExportMetadata() = fields.exportMetadata;
    // After the tracks, so the clip selected is one the track has
    if (plan.selection)
        select(fields.selectedTrack, fields.selectedClip);
}

bool ProjectStore::normalizeIds()
{
    // Only edit() and editTrack() can add or copy tracks and clips, so an idle frame costs nothing
    if (!idsDirty_)
        return false;
    idsDirty_ = false;
    settle();
    std::vector<std::vector<ObjectId>> before;
    before.reserve(project_.tracks.size());
    for (const Track &t : project_.tracks)
        before.push_back(idsOf(t));

    project_.normalizeIds(nextId_);

    bool changed = false;
    for (size_t i = 0; i < before.size(); ++i) {
        if (idsOf(project_.tracks[i]) == before[i])
            continue;
        if (!changed)
            revision_ = bump();
        changed = true;
        trackRevision_[i] = bump();
    }
    return changed;
}

uint64_t ProjectStore::revision() const
{
    settle();
    return revision_;
}

uint64_t ProjectStore::trackRevision(int track) const
{
    settle();
    if (track < 0 || static_cast<size_t>(track) >= trackRevision_.size())
        return 0;
    return trackRevision_[static_cast<size_t>(track)];
}

uint64_t ProjectStore::mixRevision(int track) const
{
    settle();
    if (track < 0 || static_cast<size_t>(track) >= mixRevision_.size())
        return 0;
    return mixRevision_[static_cast<size_t>(track)];
}

uint64_t ProjectStore::masterRevision() const
{
    settle();
    return masterRevision_;
}

uint64_t ProjectStore::selectionRevision() const
{
    settle();
    return selectionRevision_;
}

uint64_t ProjectStore::structureRevision() const
{
    settle();
    return structureRevision_;
}

void ProjectStore::settle() const
{
    const size_t count = project_.tracks.size();
    if (!structureDirty_ && trackRevision_.size() == count && mixRevision_.size() == count)
        return;
    const size_t known = trackRevision_.size();
    trackRevision_.resize(count);
    mixRevision_.resize(count);
    for (size_t i = 0; i < count; ++i) {
        if (structureDirty_ || i >= known) {
            trackRevision_[i] = bump();
            mixRevision_[i] = bump();
        }
    }
    if (structureDirty_) {
        structureRevision_ = bump();
        structureDirty_ = false;
    }
}

}
