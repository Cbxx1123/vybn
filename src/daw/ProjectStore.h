#pragma once

#include <cstdint>
#include <vector>

#include "Project.h"

namespace daw {

// The project, with every change to it passed through here. Readers get a const Project; a change
// goes through one of the edit calls, which hand out the part to change and count it as changed
// now, so the consumers can compare revision numbers (a few integers) instead of the whole project
// to learn what moved. Call an edit right when you are about to change something, and don't keep the
// reference past that edit: never store a mutable pointer or reference into the project in a member.
// An edit that ends up changing nothing only costs a re-check; a change made without an edit is a bug
// (the audio, undo and the unsaved marker would miss it). Where code runs every frame, compare first
// and edit only when the value really differs.
class ProjectStore {
public:
    explicit ProjectStore(Project project);

    ProjectStore(const ProjectStore &) = delete;
    ProjectStore &operator=(const ProjectStore &) = delete;

    // Reading
    const Project &project() const { return project_; }

    // Editing: anything (tracks added, removed or moved, project settings); every revision moves
    Project &edit();
    // A track's clips, notes, controllers, devices or kind (and anything editMix covers)
    Track &editTrack(int track);
    // ONLY a track's name, gain, pan, mute, solo, armed, monitor, inputChannel and inputStereo
    Track &editMix(int track);
    // A clip of a track (editTrack(track).clips[clip])
    Clip &editClip(int track, int clip);
    // The effects on the whole mix
    std::vector<audio::DeviceSetting> &editMasterEffects();
    // The export presets and the export details: saved, but not a track's, so only revision() moves
    std::vector<ExportPreset> &editExportPresets();
    ExportMetadata &editExportMetadata();
    // The device at a place, for changing (nullptr when there is none, which edits nothing)
    audio::DeviceSetting *editDevice(const DevicePlace &place);
    // The selection, as Project::select does it
    void select(int track, int clip = -1);
    // The tempo; edits only when it differs
    void setTempo(double bpm);
    // The loop (saved, but not undoable); edits only when it differs
    void setLoop(double start, double end, bool looping);
    // Project::toggleLoop
    void toggleLoop();
    // A whole new project (open, new, undo): every revision moves
    void replace(Project project);

    // Revisions: numbers that go up whenever that part may have changed (never down; all start at 1)
    uint64_t revision() const;                // anything that is saved (everything but the selection)
    uint64_t trackRevision(int track) const;  // its content: clips, notes, controllers, devices, kind (not editMix's fields); 0 out of range
    uint64_t mixRevision(int track) const;    // its editMix fields
    uint64_t masterRevision() const;          // the master effects
    uint64_t selectionRevision() const;       // the selection (selectedTrack/Clip; Note::selected moves with editClip too)
    uint64_t structureRevision() const;       // tracks added, removed, moved or replaced

private:
    // Sizes the per-track revisions to the project's tracks. After an edit() (which can add, remove
    // or move tracks) every track gets fresh revisions and the structure moves; a track count that
    // changed without one is sized too, but moves nothing, so the verifier can catch it
    void settle() const;
    uint64_t bump() const { return ++next_; }

    Project project_;
    mutable uint64_t next_ = 1;
    mutable uint64_t revision_ = 1;
    mutable uint64_t masterRevision_ = 1;
    mutable uint64_t selectionRevision_ = 1;
    mutable uint64_t structureRevision_ = 1;
    mutable std::vector<uint64_t> trackRevision_;
    mutable std::vector<uint64_t> mixRevision_;
    mutable bool structureDirty_ = false; // edit() was called: tracks may have been added, removed or moved
};

}
