#pragma once

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

#include "Project.h"

namespace daw {

// Undo and redo for the whole project. Rather than every edit recording itself, the history
// watches the project: once a frame DawApp hands it the project, and when what it holds differs
// from the last state recorded and the edit has settled (no button held, no text being typed),
// the last state becomes an undo step. So every edit anywhere (the piano roll, the arrangement,
// the mixer, the inspector, imports, the browser) can be undone without knowing about it.
// Steps are snapshots of the whole project in which each clip (with its notes, the bulk of a
// project) is shared with the neighbouring step when it didn't change, so a step costs about
// what it changed rather than a copy of every note.
//
// What counts as a change is the content: the tracks and their clips, notes and mix, the tempo
// and the project's settings. The selection (tracks, clips, notes) isn't a change on its own,
// but a step remembers the selection it had, and arming and monitoring aren't part of it at all.
class History {
public:
    static constexpr size_t kMaxSteps = 200;

    // Forgets every step; `project` is where it starts from (a project opened or made)
    void reset(const Project &project);
    // Once a frame. Records a step when the project's content has changed since the last state
    // recorded and `settled` is set; until then the change waits. `canMerge` lets the step join
    // the last one when it is the same kind of change within half a second (holding a key that
    // transposes, turning a knob with the wheel). `now` is in seconds. Returns whether it recorded.
    bool update(const Project &project, bool settled, bool canMerge = false, double now = 0.0);

    bool canUndo() const { return !undo_.empty(); }
    bool canRedo() const { return !redo_.empty(); }
    // What the next undo or redo does ("Delete notes"), or empty
    std::string undoLabel() const { return undo_.empty() ? std::string() : undo_.back().label; }
    std::string redoLabel() const { return redo_.empty() ? std::string() : redo_.back().label; }
    size_t undoCount() const { return undo_.size(); }
    size_t redoCount() const { return redo_.size(); }
    // The last state recorded
    const Project &current() const { return state_; }
    // Clips held by the steps (each shared one counted once), for tests and figures
    size_t storedClips() const;

    // The state to put back, or null when there's nothing to undo. `current` is the project as it
    // is now: a change still waiting to settle is recorded first, so undo takes that back.
    const Project *undo(const Project &current, double now = 0.0);
    const Project *redo(const Project &current);

private:
    // A project with its clips taken out of the tracks and held by pointer, shared between
    // snapshots wherever a clip is the same
    struct Snapshot {
        Project shell;
        std::vector<std::vector<std::shared_ptr<const Clip>>> clips; // by track
    };
    struct Step {
        Snapshot snapshot;
        std::string label;
    };
    // Packs `project`, reusing the clips `like` has at the same places when they are identical
    static Snapshot pack(Project project, const Snapshot *like);
    static Project unpack(const Snapshot &snapshot);

    Project state_; // the last state recorded
    std::vector<Step> undo_;
    std::vector<Step> redo_;
    double lastTime_ = -1e9;
};

// Whether two projects have the same content, as the history sees it: not the selection, not
// arming or monitoring
bool sameContent(const Project &a, const Project &b);
// Whether they also have the same selection (tracks, clips and notes)
bool sameSelection(const Project &a, const Project &b);
// Whether writeProject would write the same text for both: the content, plus what the file keeps
// that sameContent doesn't (arming, monitoring, the inputs, the loop, plugin states). Not the
// selection, which the file doesn't keep either. Each field is compared only where the file writes it.
bool sameSaved(const Project &a, const Project &b);
// A few words for what changed from `before` to `after`, for "Undo ..." ("Move clip", "Edit notes")
std::string describeChange(const Project &before, const Project &after);

}
