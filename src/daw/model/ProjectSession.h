#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "daw/model/History.h"
#include "daw/model/Project.h"
#include "daw/model/ProjectStore.h"

namespace daw {

// The document the app is editing: the project model (in a ProjectStore, which counts its changes),
// the .vbp file it was opened from or saved to, whether it differs from what was last saved (the
// saved text is kept to compare against), and its undo history. The starting project counts as
// saved. The project's address never changes (replace assigns into it), so the views and the audio
// side can hold a reference to it.
class ProjectSession {
public:
    // The starting project counts as saved, with no file yet
    explicit ProjectSession(Project project);

    ProjectSession(const ProjectSession &) = delete;
    ProjectSession &operator=(const ProjectSession &) = delete;

    // The store every change goes through (see ProjectStore)
    ProjectStore &store() { return store_; }
    const ProjectStore &store() const { return store_; }

    // The project as it is now, for reading (changes go through store())
    const Project &project() const { return store_.project(); }

    // The .vbp file the project was opened from or saved to, empty until it has one
    const std::string &path() const { return path_; }

    // Whether the project differs from what was last saved or opened (as of the last checkModified)
    bool modified() const { return modified_; }
    // Compares the project as it would be saved with the saved text: once a few times a second
    void checkModified();

    // Safety net for the store (off unless the environment variable VIBIN_VERIFY_STORE is 1): once
    // every few frames, a change to the project that its revisions didn't count is printed to stderr
    void verify();

    // After a successful save: the project is now the file at `path`, and it is not modified
    void markSaved(const std::string &path);

    // A project opened or made: it becomes the document, saved, with no file of the given path
    // (empty for a new one), and the history starts again from it. With `saved` (a recovered or
    // restored project), that is what the file holds instead, so the project reads as modified when
    // it differs from it.
    void replace(Project project, std::string path, const Project *saved = nullptr);

    // The undo history of the project's edits (see History). Changes to it go through recordHistory.
    History &history() { return history_; }
    const History &history() const { return history_; }

    // Once a frame: records the undo step for the project's changes (see History::update). Nothing is
    // compared unless a revision moved since the last call, so an idle frame costs two integer reads.
    // Returns whether a step was recorded.
    bool recordHistory(bool settled, bool canMerge, double now);

    // Puts back what the history planned (undo or redo): the store applies it through its edit calls, so
    // only what differs moves (see ProjectStore::restore). Arming, monitoring and the loop stay as they
    // are now, since they aren't part of the history. The saved text stays as it is too. When nothing
    // else changed the project since the plan was made, the history adopts the store's revisions, so
    // the next frame has nothing to compare.
    void restoreState(HistoryRestore plan);
    // A whole project as the state to put back (replacing the current one), for tests
    void restoreState(Project state);

private:
    ProjectStore store_;
    std::string path_;      // the .vbp file, empty until saved
    Project savedCopy_;     // the project as last saved or opened, to tell when it has changed (sameSaved)
    bool modified_ = false;
    History history_;

    // The revisions checkModified and recordHistory last saw: checkModified compares the saved text
    // only when the project's revision moved since, and recordHistory runs its compare only while
    // dirty_ (an edit or a selection change is waiting for the history to look at it)
    uint64_t checkedRevision_ = 0;
    uint64_t seenRevision_ = 0;
    uint64_t seenSelection_ = 0;
    bool dirty_ = true;

    // The verifier's last look: a copy of the project, its saved text, and the revisions it had
    bool verified_ = false;
    Project verifiedProject_;
    std::string verifiedText_;
    uint64_t verifiedRevision_ = 0;
    uint64_t verifiedStructure_ = 0;
    std::vector<uint64_t> verifiedTracks_;
};

}
