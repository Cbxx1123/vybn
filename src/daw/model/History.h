#pragma once

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

#include "daw/model/Project.h"

namespace daw {

class ProjectStore;

// What an undo or redo changes, worked out from the two states' parts rather than by copying the
// project: ProjectStore::restore applies it through the store's own edit calls, so only what really
// differs has its revisions move. Tracks that come, go, move or change kind can't be applied that
// way: then `structural` is set, `project` is the whole project to put back, and it replaces the
// current one. Otherwise `project` holds the project's own fields (no tracks) and the flags say which
// of them to set, and `tracks` lists the tracks that differ, each as the track to put back with
// whether its content (clips, devices) and its mix (name, gain, pan, mute, solo, input) changed.
// Arming and monitoring are never part of it, and nor is the loop.
struct HistoryRestore {
    struct TrackChange {
        size_t index = 0;
        Track track;
        bool content = false;
        bool mix = false;
    };
    std::string label; // what it undoes or redoes ("Delete notes")
    bool structural = false;
    Project project;
    bool timing = false;    // the tempo map differs (a structural plan always sets it)
    bool settings = false;  // name or length differ
    bool markers = false;   // the markers differ
    bool presets = false;   // export presets differ
    bool metadata = false;  // export details differ
    bool master = false;    // the master's effects differ
    bool selection = false; // the selected track or clip differs
    std::vector<TrackChange> tracks;
    // The store's revision() when the plan was made: if it has moved by the time the plan is applied,
    // something else changed the project in between, and the history can't take the result as its own
    uint64_t revision = 0;
};

// Undo and redo for the whole project. Rather than every edit recording itself, the history
// watches the project: once a frame DawApp hands it the store, and when what it holds differs
// from the last state recorded and the edit has settled (no button held, no text being typed),
// the last state becomes an undo step. So every edit anywhere (the piano roll, the arrangement,
// the mixer, the inspector, imports, the browser) can be undone without knowing about it.
//
// Steps are snapshots held in two levels, so a step costs about what it changed rather than a copy
// of every note: the project's own fields (tempo map, master effects, settings) are copied, and each
// track is a shell (everything but its clips: name, mix, devices...) plus its clips, each held by
// pointer and shared with the neighbouring snapshot wherever it didn't change. The last state
// recorded is kept in the same form, so a selection change or an edit still waiting to settle
// never copies the whole project.
//
// The history also remembers the store's revisions as of the last state recorded. A track whose
// revisions haven't moved since is known to be the same, so it isn't compared and keeps its
// pointers: a frame costs what changed, not the size of the project. Until the tracks come, go
// or move (the structure), only the tracks whose revisions moved are compared. Once an edit has
// been seen to differ while it is still unsettled, nothing more is compared until it settles.
//
// Undo and redo don't hand back a project: they return a HistoryRestore, the parts that differ between
// the state recorded and the step, which ProjectStore::restore applies through the store's edit calls. So
// undoing a one-note edit moves one track's revisions, not every track's.
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
    // This one looks at the store's revisions to find what to compare.
    bool update(const ProjectStore &store, bool settled, bool canMerge = false, double now = 0.0);
    // The same for a project on its own, with no revisions: every track is compared. For tests, and
    // for undo, which has only the project.
    bool update(const Project &project, bool settled, bool canMerge = false, double now = 0.0);

    bool canUndo() const { return !undo_.empty(); }
    bool canRedo() const { return !redo_.empty(); }
    // What the next undo or redo does ("Delete notes"), or empty
    std::string undoLabel() const { return undo_.empty() ? std::string() : undo_.back().label; }
    std::string redoLabel() const { return redo_.empty() ? std::string() : redo_.back().label; }
    size_t undoCount() const { return undo_.size(); }
    size_t redoCount() const { return redo_.size(); }
    // Whether `project` has the same content as the last state recorded (a change still waiting to
    // settle makes it differ)
    bool matches(const Project &project) const;
    // The last state recorded, put together from its parts on each call (so not for every frame)
    const Project &current() const;
    // Clips held by the steps (each shared one counted once), for tests and figures
    size_t storedClips() const;
    // A rough count of the bytes the steps hold: each distinct clip with its notes and each distinct
    // track shell once, plus the pointers and project fields of every step
    size_t storedBytes() const;

    // The plan to put back the state before the last step, or false when there's nothing to undo. A
    // change still waiting to settle is recorded first, so undo takes that back. Apply the plan with
    // ProjectStore::restore, then call adopt. Redo is the same for the step undone last; it refuses
    // once an edit has replaced what could be redone (judged by the store's revisions).
    bool undo(const ProjectStore &store, double now, HistoryRestore &plan);
    bool redo(const ProjectStore &store, HistoryRestore &plan);
    // After a plan has been applied to `store` and nothing else changed it: the store now holds the
    // state, with these revisions, so the next look finds nothing to compare
    void adopt(const ProjectStore &store);
    // The same on a project alone, for tests: the whole state to put back, or null
    const Project *undo(const Project &current, double now = 0.0);
    const Project *redo(const Project &current);

private:
    // A track without its clips, and its clips held by pointer
    struct TrackEntry {
        std::shared_ptr<const Track> shell;
        std::vector<std::shared_ptr<const Clip>> clips;
    };
    // A project whose tracks are taken out of it (`shell` has none) and held as entries, shared
    // between snapshots wherever a track or a clip is the same
    struct Snapshot {
        Project shell;
        std::vector<TrackEntry> tracks;
    };
    struct Step {
        Snapshot snapshot;
        std::string label;
    };

    // What a look has to compare, from the revisions (all tracks and fields when there are none)
    struct Compare {
        bool differs = false;
        bool structure = true, fields = true, master = true;
        std::vector<char> moved;
    };
    Compare compare(const Project &project, const ProjectStore *store) const;
    // The core of both updates; `store` is null when there are no revisions to go by
    bool look(const Project &project, const ProjectStore *store, bool settled, bool canMerge, double now);
    // One undo (or, with `forward`, redo): moves the state and the step; with a `plan`, works out what
    // changed between them
    bool takeStep(bool forward, const Project &current, const ProjectStore *store, double now, HistoryRestore *plan);
    static HistoryRestore makePlan(const Snapshot &from, const Snapshot &to, const std::string &label);
    // Brings the last state recorded up to `project`: the tracks that `moved` (all of them when the
    // structure did, matched to the old ones by id so unchanged clips are still shared), the
    // project's own fields when `fields` (the master's effects only when `master`), and the selection
    void sync(const Project &project, const std::vector<char> &moved, bool structure, bool fields, bool master);
    // Remembers the store's revisions as the ones the last state recorded has (or, with no store,
    // that they aren't known, so the next look compares everything)
    void markSeen(const ProjectStore *store);
    static bool sameEntry(const TrackEntry &entry, const Track &track);
    static void syncEntry(TrackEntry &entry, const Track &track);
    static Track unpackTrack(const TrackEntry &entry);
    static Project unpack(const Snapshot &snapshot);

    Snapshot state_; // the last state recorded
    std::vector<Step> undo_;
    std::vector<Step> redo_;
    double lastTime_ = -1e9;

    // The revisions state_ is as of (valid only while known_), so what moved since is what to compare
    bool known_ = false;
    uint64_t structureSeen_ = 0, revisionSeen_ = 0, masterSeen_ = 0;
    std::vector<uint64_t> trackSeen_, mixSeen_;
    // A content difference has been seen and is waiting to settle: nothing more is compared meanwhile
    bool pending_ = false;
    mutable Project currentCache_; // what current() returns
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
