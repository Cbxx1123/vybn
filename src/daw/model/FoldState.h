#pragma once

#include <cstdint>
#include <set>

#include "daw/model/Project.h"
#include "daw/model/ProjectStore.h"
#include "daw/model/Routing.h"

namespace daw {

// Which group tracks are folded in the arrangement and the mixer (docs/routing-next-plan.md, group
// folding). The state is the project's own: `Track::folded` on a group, saved with the project and,
// like record arming, not an undo step (folding goes through `editMix`, so it marks the project
// modified). This is the view of it the arrangement and the mixer share: `set` writes through the
// store, and `version` moves whenever the set of folded groups changes (by folding here, in another
// view, or by a project opened or an undo that brings a group back), which is how the mixer, whose
// strips are tree children, knows to rebuild. The version is worked out only when the store's
// revision has moved, so reading it every frame costs a compare.
class FoldState {
public:
    explicit FoldState(ProjectStore &store) : store_(store), shownSelection_(store.selectionRevision()) {}

    bool folded(ObjectId group) const
    {
        const Track *t = store_.project().findTrack(group);
        return t && t->kind == TrackKind::Group && t->folded;
    }
    // Folds or unfolds a group (nothing for any other track); true when that changed anything
    bool set(ObjectId group, bool folded)
    {
        const Track *t = store_.project().findTrack(group);
        if (!t || t->kind != TrackKind::Group || t->folded == folded)
            return false;
        store_.editMix(store_.project().trackIndex(group)).folded = folded;
        return true;
    }
    // Unfolds every group
    void clear()
    {
        for (int i = 0; i < static_cast<int>(store_.project().tracks.size()); ++i) {
            if (store_.project().tracks[static_cast<size_t>(i)].folded)
                store_.editMix(i).folded = false;
        }
    }
    // The ids of the folded groups
    std::set<ObjectId> groups() const
    {
        std::set<ObjectId> out;
        for (const Track &t : store_.project().tracks) {
            if (t.kind == TrackKind::Group && t.folded)
                out.insert(t.id);
        }
        return out;
    }
    uint64_t version() const
    {
        if (seen_ != store_.revision()) {
            seen_ = store_.revision();
            uint64_t hash = 1469598103934665603ull; // FNV-1a over the folded ids, in track order
            for (const Track &t : store_.project().tracks) {
                if (t.kind == TrackKind::Group && t.folded)
                    hash = (hash ^ t.id) * 1099511628211ull;
            }
            if (hash != hash_) {
                hash_ = hash;
                ++version_;
            }
        }
        return version_;
    }

    // A selection that lands on a hidden track (the mixer, the keyboard, an undo) unfolds the groups
    // hiding it, once per change of the selection. The views call it every frame, so it works with
    // either open alone; true when it unfolded anything.
    bool revealSelection()
    {
        if (store_.selectionRevision() == shownSelection_)
            return false;
        shownSelection_ = store_.selectionRevision();
        bool changed = false;
        const Project &project = store_.project();
        for (int g : outputGroups(project, project.selectedTrack))
            changed = set(project.tracks[static_cast<size_t>(g)].id, false) || changed;
        return changed;
    }

private:
    ProjectStore &store_;
    uint64_t shownSelection_ = 0; // the store's selectionRevision revealSelection last looked at
    mutable uint64_t seen_ = 0;  // the store's revision the hash was taken at
    mutable uint64_t hash_ = 0;
    mutable uint64_t version_ = 1;
};

}
