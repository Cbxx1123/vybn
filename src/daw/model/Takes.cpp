#include "daw/model/Takes.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <set>

namespace daw {

namespace {

constexpr double kEdge = 1e-6; // beats: edges this close meet

void sortClips(std::vector<Clip> &clips)
{
    std::stable_sort(clips.begin(), clips.end(), [](const Clip &a, const Clip &b) { return a.start < b.start; });
}

// Two sections of one take that meet, and play on from each other: one clip
bool canMerge(const Clip &a, const Clip &b, const audio::TempoMap &timing)
{
    if (a.take == kNoId || a.take != b.take || std::fabs(a.end() - b.start) > kEdge || a.audioFile != b.audioFile ||
        a.gain != b.gain || a.warp != b.warp)
        return false;
    const double expected = clipFileSeconds(timing, a, b.start);
    return !a.hasAudio() || std::fabs(b.offset - expected) < 1e-6;
}

void merge(Clip &a, const Clip &b)
{
    const double shift = b.start - a.start;
    for (Note n : b.notes) {
        n.start += shift;
        a.notes.push_back(n);
    }
    for (const ControlLane &lane : b.controls) {
        auto same = std::find_if(a.controls.begin(), a.controls.end(), [&](const ControlLane &l) { return l.controller == lane.controller; });
        if (same == a.controls.end()) {
            a.controls.push_back({lane.controller, {}});
            same = a.controls.end() - 1;
        } else if (!same->points.empty() && same->points.back().beat >= a.length - 1e-9) {
            same->points.pop_back(); // the held value at a's end: b's first point takes over there
        }
        for (const ControlPoint &p : lane.points)
            same->points.push_back({p.beat + shift, p.value});
    }
    a.length = b.end() - a.start;
    a.fadeOut = b.fadeOut;
}

void mergeSections(std::vector<Clip> &clips, const audio::TempoMap &timing)
{
    sortClips(clips);
    std::vector<Clip> out;
    for (Clip &c : clips) {
        if (!out.empty() && canMerge(out.back(), c, timing))
            merge(out.back(), c);
        else
            out.push_back(std::move(c));
    }
    clips = std::move(out);
}

bool overlaps(const Clip &c, double from, double to)
{
    return c.start < to - kEdge && c.end() > from + kEdge;
}

}

Clip sliceClip(const Clip &clip, double from, double to, const audio::TempoMap &timing)
{
    from = std::max(from, clip.start);
    to = std::min(to, clip.end());
    if (from <= clip.start + 1e-9 && to >= clip.end() - 1e-9)
        return clip;
    to = std::max(to, from);
    Clip s = clip;
    const double into = from - clip.start;
    s.start = from;
    s.length = to - from;
    s.offset = clipFileSeconds(timing, clip, from);
    s.fadeIn = into < 1e-9 ? clip.fadeIn : 0.0;
    s.fadeOut = to >= clip.end() - 1e-9 ? clip.fadeOut : 0.0;
    s.notes.clear();
    for (Note n : clip.notes) {
        if (n.start >= into - 1e-9 && n.start < into + s.length - 1e-9) {
            n.start = std::max(0.0, n.start - into);
            s.notes.push_back(n);
        }
    }
    s.controls.clear();
    for (const ControlLane &lane : clip.controls) {
        ControlLane cut{lane.controller, {}};
        if (!lane.points.empty()) {
            cut.points.push_back({0.0, lane.valueAt(into)});
            for (const ControlPoint &p : lane.points) {
                if (p.beat > into + 1e-9 && p.beat < into + s.length - 1e-9)
                    cut.points.push_back({p.beat - into, p.value});
            }
            cut.points.push_back({s.length, lane.valueAt(into + s.length)});
        }
        s.controls.push_back(std::move(cut));
    }
    return s;
}

const Clip *findTake(const Track &track, ObjectId take, size_t *lane)
{
    if (take == kNoId)
        return nullptr;
    for (size_t l = 0; l < track.takeLanes.size(); ++l) {
        for (const Clip &c : track.takeLanes[l].clips) {
            if (c.id == take) {
                if (lane)
                    *lane = l;
                return &c;
            }
        }
    }
    return nullptr;
}

const TakeLane *findLane(const Track &track, ObjectId lane)
{
    for (const TakeLane &l : track.takeLanes) {
        if (l.id == lane)
            return &l;
    }
    return nullptr;
}

TakeLane *findLane(Track &track, ObjectId lane)
{
    return const_cast<TakeLane *>(findLane(static_cast<const Track &>(track), lane));
}

int laneOfSection(const Track &track, const Clip &section)
{
    size_t lane = 0;
    return findTake(track, section.take, &lane) ? static_cast<int>(lane) : -1;
}

std::string nextTakeName(const Track &track)
{
    int highest = 0;
    for (const TakeLane &lane : track.takeLanes) {
        if (lane.name.rfind("Take ", 0) == 0)
            highest = std::max(highest, std::atoi(lane.name.c_str() + 5));
    }
    return "Take " + std::to_string(highest + 1);
}

bool compRange(Track &track, double from, double to, ObjectId take, const audio::TempoMap &timing)
{
    const Clip *source = findTake(track, take);
    if (!source)
        return false;
    from = std::max(from, source->start);
    to = std::min(to, source->end());
    if (to - from < kMinSection - 1e-9)
        return false;
    Clip section = sliceClip(*source, from, to, timing);
    section.id = kNoId;
    section.take = take;
    // Already so: nothing to do (and nothing counted as an edit)
    for (const Clip &c : track.clips) {
        if (c.take == take && c.start <= from + kEdge && c.end() >= to - kEdge &&
            std::fabs(c.offset - clipFileSeconds(timing, *source, c.start)) < 1e-6)
            return false;
    }
    std::vector<Clip> clips;
    clips.reserve(track.clips.size() + 2);
    for (const Clip &c : track.clips) {
        if (!overlaps(c, from, to)) {
            clips.push_back(c);
            continue;
        }
        const bool left = c.start < from - kEdge;
        if (left)
            clips.push_back(sliceClip(c, c.start, from, timing));
        if (c.end() > to + kEdge) {
            Clip right = sliceClip(c, to, c.end(), timing);
            if (left)
                right.id = kNoId; // the left piece kept the id
            clips.push_back(std::move(right));
        }
    }
    clips.push_back(std::move(section));
    mergeSections(clips, timing);
    track.clips = std::move(clips);
    return true;
}

std::pair<double, double> sectionAround(const Track &track, double beat)
{
    double before = 0.0, after = 1e12;
    for (const Clip &c : track.clips) {
        if (c.start <= beat && c.end() > beat)
            return {c.start, c.end()};
        if (c.end() <= beat)
            before = std::max(before, c.end());
        if (c.start > beat)
            after = std::min(after, c.start);
    }
    return {before, after};
}

bool useLane(Track &track, ObjectId lane, const audio::TempoMap &timing)
{
    const TakeLane *found = findLane(track, lane);
    if (!found)
        return false;
    const std::vector<Clip> takes = found->clips; // compRange changes the track's clips, not its lanes
    bool changed = false;
    for (const Clip &take : takes)
        changed = compRange(track, take.start, take.end(), take.id, timing) || changed;
    return changed;
}

bool newestComp(Track &track, double from, double to, const audio::TempoMap &timing)
{
    bool changed = false;
    // The oldest first, so each newer take covers it where it reaches
    for (size_t l = track.takeLanes.size(); l-- > 0;) {
        const std::vector<Clip> takes = track.takeLanes[l].clips;
        for (const Clip &take : takes) {
            if (overlaps(take, from, to))
                changed = compRange(track, std::max(from, take.start), std::min(to, take.end()), take.id, timing) || changed;
        }
    }
    return changed;
}

bool moveBoundary(Track &track, double at, double to, const audio::TempoMap &timing)
{
    const Clip *left = nullptr, *right = nullptr;
    for (const Clip &c : track.clips) {
        if (std::fabs(c.end() - at) < kEdge)
            left = &c;
        if (std::fabs(c.start - at) < kEdge)
            right = &c;
    }
    if (to > at && left && left->take != kNoId) {
        // The left section grows; the right one keeps at least the shortest section
        if (right)
            to = std::min(to, right->end() - kMinSection);
        const ObjectId take = left->take;
        return to > at + 1e-9 && compRange(track, at, to, take, timing);
    }
    if (to < at && right && right->take != kNoId) {
        if (left)
            to = std::max(to, left->start + kMinSection);
        const ObjectId take = right->take;
        return to < at - 1e-9 && compRange(track, to, at, take, timing);
    }
    return false;
}

bool stepTake(Track &track, size_t index, int step, const audio::TempoMap &timing)
{
    if (index >= track.clips.size() || track.takeLanes.empty())
        return false;
    const Clip section = track.clips[index];
    const int lanes = static_cast<int>(track.takeLanes.size());
    // An ordinary clip steps into the lanes from the top (or the bottom, going up)
    int lane = laneOfSection(track, section);
    if (lane < 0)
        lane = step > 0 ? -1 : lanes;
    for (int l = lane + step; l >= 0 && l < lanes; l += step) {
        for (const Clip &take : track.takeLanes[static_cast<size_t>(l)].clips) {
            if (overlaps(take, section.start, section.end()))
                return compRange(track, section.start, section.end(), take.id, timing);
        }
    }
    return false;
}

std::vector<std::pair<double, double>> laneUse(const Track &track, ObjectId lane)
{
    std::vector<std::pair<double, double>> used;
    const TakeLane *found = findLane(track, lane);
    if (!found)
        return used;
    std::set<ObjectId> takes;
    for (const Clip &c : found->clips)
        takes.insert(c.id);
    for (const Clip &c : track.clips) {
        if (takes.count(c.take))
            used.push_back({c.start, c.end()});
    }
    std::sort(used.begin(), used.end());
    return used;
}

std::vector<double> compBoundaries(const Track &track)
{
    std::vector<const Clip *> sorted;
    for (const Clip &c : track.clips)
        sorted.push_back(&c);
    std::stable_sort(sorted.begin(), sorted.end(), [](const Clip *a, const Clip *b) { return a->start < b->start; });
    std::vector<double> edges;
    for (size_t i = 0; i + 1 < sorted.size(); ++i) {
        if (std::fabs(sorted[i]->end() - sorted[i + 1]->start) < kEdge &&
            (sorted[i]->take != kNoId || sorted[i + 1]->take != kNoId))
            edges.push_back(sorted[i + 1]->start);
    }
    return edges;
}

bool flattenComp(Track &track)
{
    bool changed = !track.takeLanes.empty();
    track.takeLanes.clear();
    for (Clip &c : track.clips) {
        changed = changed || c.take != kNoId;
        c.take = kNoId;
    }
    return changed;
}

bool deleteUnusedTakes(Track &track)
{
    std::set<ObjectId> used;
    for (const Clip &c : track.clips)
        used.insert(c.take);
    bool changed = false;
    for (TakeLane &lane : track.takeLanes) {
        const size_t before = lane.clips.size();
        lane.clips.erase(std::remove_if(lane.clips.begin(), lane.clips.end(), [&](const Clip &c) { return !used.count(c.id); }),
                         lane.clips.end());
        changed = changed || lane.clips.size() != before;
    }
    const size_t lanes = track.takeLanes.size();
    track.takeLanes.erase(std::remove_if(track.takeLanes.begin(), track.takeLanes.end(),
                                         [](const TakeLane &l) { return l.clips.empty(); }),
                          track.takeLanes.end());
    return changed || track.takeLanes.size() != lanes;
}

bool removeLane(Track &track, ObjectId lane)
{
    const TakeLane *found = findLane(track, lane);
    if (!found)
        return false;
    std::set<ObjectId> takes;
    for (const Clip &c : found->clips)
        takes.insert(c.id);
    for (Clip &c : track.clips) {
        if (takes.count(c.take))
            c.take = kNoId;
    }
    track.takeLanes.erase(std::remove_if(track.takeLanes.begin(), track.takeLanes.end(),
                                         [lane](const TakeLane &l) { return l.id == lane; }),
                          track.takeLanes.end());
    return true;
}

bool foldTakes(Track &track, std::vector<Clip> passes, const audio::TempoMap &timing)
{
    if (passes.empty())
        return false;
    double from = passes.front().start, to = passes.front().end();
    for (const Clip &p : passes) {
        from = std::min(from, p.start);
        to = std::max(to, p.end());
    }
    // What the recording went over: ordinary clips, and sections of earlier takes
    std::vector<Clip> under;
    std::vector<char> isUnder(track.clips.size(), 0);
    bool sectionsUnder = false;
    for (size_t i = 0; i < track.clips.size(); ++i) {
        const Clip &c = track.clips[i];
        if (!overlaps(c, from, to))
            continue;
        if (c.take == kNoId) {
            under.push_back(c);
            isUnder[i] = 1;
        } else {
            sectionsUnder = true;
        }
    }
    // One pass over empty space is an ordinary clip, as it always was
    if (passes.size() == 1 && under.empty() && !sectionsUnder) {
        passes.front().take = kNoId;
        track.clips.push_back(std::move(passes.front()));
        sortClips(track.clips);
        return false;
    }

    // Fresh lane ids, above every id the track holds (normalizeIds makes them unique in the project)
    ObjectId next = track.id + 1;
    for (const Clip &c : track.clips)
        next = std::max(next, c.id + 1);
    for (const TakeLane &lane : track.takeLanes) {
        next = std::max(next, lane.id + 1);
        for (const Clip &c : lane.clips)
            next = std::max(next, c.id + 1);
    }
    for (const Clip &p : passes)
        next = std::max(next, p.id + 1);

    // The passes, the newest first, numbered on from the track's takes
    const std::string name = nextTakeName(track);
    int number = std::atoi(name.c_str() + 5);
    std::vector<TakeLane> takes;
    for (Clip &p : passes) {
        p.take = kNoId;
        p.fadeIn = p.fadeOut = 0.0;
        if (p.id == kNoId)
            p.id = next++;
        p.name = "Take " + std::to_string(number++);
        const std::string laneName = p.name;
        takes.push_back({next++, laneName, {std::move(p)}});
    }
    std::reverse(takes.begin(), takes.end());
    std::vector<TakeLane> fresh = std::move(takes);
    // What was recorded over keeps a lane of its own, below the new takes
    if (!under.empty()) {
        TakeLane original{next++, "Original", {}};
        for (Clip c : under) {
            from = std::min(from, c.start);
            to = std::max(to, c.end());
            if (c.id == kNoId)
                c.id = next++;
            original.clips.push_back(std::move(c));
        }
        sortClips(original.clips);
        std::vector<Clip> kept;
        for (size_t i = 0; i < track.clips.size(); ++i) {
            if (!isUnder[i])
                kept.push_back(std::move(track.clips[i]));
        }
        track.clips = std::move(kept);
        fresh.push_back(std::move(original));
    }
    track.takeLanes.insert(track.takeLanes.begin(), std::make_move_iterator(fresh.begin()), std::make_move_iterator(fresh.end()));
    newestComp(track, from, to, timing);
    return true;
}

}
