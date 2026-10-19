#include "ProjectStore.h"

#include <utility>

namespace daw {

ProjectStore::ProjectStore(Project project)
    : project_(std::move(project)), trackRevision_(project_.tracks.size(), 1), mixRevision_(project_.tracks.size(), 1)
{
}

Project &ProjectStore::edit()
{
    settle();
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

void ProjectStore::setTempo(double bpm)
{
    settle();
    if (project_.tempo == bpm)
        return;
    project_.tempo = bpm;
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
