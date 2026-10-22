#include "ProjectSession.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <utility>

#include "ProjectFile.h"

namespace daw {

namespace {

bool sameNoteContent(const Note &a, const Note &b)
{
    return a.start == b.start && a.length == b.length && a.key == b.key && a.velocity == b.velocity &&
           a.muted == b.muted; // not the selection
}

bool sameClipContent(const Clip &a, const Clip &b)
{
    if (a.name != b.name || a.start != b.start || a.length != b.length || a.audioFile != b.audioFile ||
        a.offset != b.offset || a.gain != b.gain || a.fadeIn != b.fadeIn || a.fadeOut != b.fadeOut ||
        a.notes.size() != b.notes.size() || a.controls.size() != b.controls.size())
        return false;
    for (size_t i = 0; i < a.notes.size(); ++i) {
        if (!sameNoteContent(a.notes[i], b.notes[i]))
            return false;
    }
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
    return true;
}

// A track's content as the store's track revision counts it: clips, notes, controllers, devices, automation and
// kind, but not the mix fields (name, gain, pan, mute, solo, armed, folded, monitor, input)
bool sameTrackContent(const Track &a, const Track &b)
{
    if (a.kind != b.kind || a.instrument != b.instrument || a.effects != b.effects || a.automation != b.automation ||
        a.clips.size() != b.clips.size())
        return false;
    for (size_t i = 0; i < a.clips.size(); ++i) {
        if (!sameClipContent(a.clips[i], b.clips[i]))
            return false;
    }
    return true;
}

}

ProjectSession::ProjectSession(Project project)
    : store_(std::move(project)), savedCopy_(store_.project())
{
}

void ProjectSession::checkModified()
{
    // Nothing changed since the last check: the saved text can't have changed either
    const uint64_t revision = store_.revision();
    if (revision == checkedRevision_)
        return;
    modified_ = !sameSaved(store_.project(), savedCopy_);
    checkedRevision_ = revision;
}

bool ProjectSession::recordHistory(bool settled, bool canMerge, double now)
{
    // An edit or a selection change since the last call makes the history look again; until one
    // happens, the project is the one the history already holds
    const uint64_t revision = store_.revision();
    const uint64_t selection = store_.selectionRevision();
    if (revision != seenRevision_ || selection != seenSelection_) {
        seenRevision_ = revision;
        seenSelection_ = selection;
        dirty_ = true;
    }
    if (!dirty_)
        return false;
    const bool recorded = history_.update(store_, settled, canMerge, now);
    // A change still waiting for the edit to settle stays dirty, so the next settled frame looks again
    if (settled)
        dirty_ = false;
    return recorded;
}

void ProjectSession::restoreState(HistoryRestore plan)
{
    // The plan was made against the project as it was; if something has changed it since, the history
    // can't take the result as its own and looks again next frame
    const bool untouched = plan.revision != 0 && store_.revision() == plan.revision;
    store_.restore(std::move(plan));
    if (untouched) {
        history_.adopt(store_);
        seenRevision_ = store_.revision();
        seenSelection_ = store_.selectionRevision();
        dirty_ = false;
    } else {
        dirty_ = true;
    }
}

void ProjectSession::restoreState(Project state)
{
    HistoryRestore plan;
    plan.structural = true;
    plan.project = std::move(state);
    restoreState(std::move(plan));
}

void ProjectSession::verify()
{
    static const bool enabled = [] {
        const char *value = std::getenv("VIBIN_VERIFY_STORE");
        return value && std::string(value) == "1";
    }();
    if (!enabled)
        return;

    const Project &now = store_.project();
    const uint64_t revision = store_.revision();
    const uint64_t structure = store_.structureRevision();
    const std::string text = writeProject(now);
    if (verified_) {
        if (revision == verifiedRevision_ && text != verifiedText_) {
            std::fprintf(stderr, "ProjectStore: the project changed without an edit\n");
            std::fflush(stderr);
        }
        if (structure == verifiedStructure_) {
            const size_t count = std::min(now.tracks.size(), verifiedProject_.tracks.size());
            for (size_t i = 0; i < count && i < verifiedTracks_.size(); ++i) {
                if (store_.trackRevision(static_cast<int>(i)) != verifiedTracks_[i])
                    continue;
                if (!sameTrackContent(now.tracks[i], verifiedProject_.tracks[i])) {
                    std::fprintf(stderr, "ProjectStore: track %zu's content changed without an edit\n", i);
                    std::fflush(stderr);
                }
            }
        }
    }

    verifiedProject_ = now;
    verifiedText_ = text;
    verifiedRevision_ = revision;
    verifiedStructure_ = structure;
    verifiedTracks_.clear();
    for (size_t i = 0; i < now.tracks.size(); ++i)
        verifiedTracks_.push_back(store_.trackRevision(static_cast<int>(i)));
    verified_ = true;
}

void ProjectSession::markSaved(const std::string &path)
{
    path_ = path;
    savedCopy_ = store_.project();
    modified_ = false;
    checkedRevision_ = store_.revision();
}

void ProjectSession::replace(Project project, std::string path, const Project *saved)
{
    store_.replace(std::move(project));
    path_ = std::move(path);
    savedCopy_ = saved ? *saved : store_.project();
    modified_ = saved ? !sameSaved(store_.project(), savedCopy_) : false;
    checkedRevision_ = store_.revision();
    history_.reset(store_.project());
    dirty_ = true; // the history starts from this project; the next frame compares it once
}

}
