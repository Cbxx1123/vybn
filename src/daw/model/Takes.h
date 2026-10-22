#pragma once

#include <string>
#include <utility>
#include <vector>

#include "daw/model/Project.h"

// Takes and comping (docs/design/takes-plan.md). A track's take lanes keep recorded passes, which
// don't play; its own clips are the comp, each comp section an ordinary clip cut from a take
// (Clip::take). These are the rules for building the comp, on one track; positions in beats, with
// the tempo map for audio offsets. Each returns whether it changed anything.

namespace daw {

// The shortest comp section made (an eighth of a 1/16th)
constexpr double kMinSection = 1.0 / 32.0;

// A clip cut down to [from, to) (clamped to it): an audio clip's offset follows its new start, notes
// that start in the range are kept (moved to the new start), controller curves keep their values at
// the edges, and fades only stay at an edge that is the clip's own. The id is kept.
Clip sliceClip(const Clip &clip, double from, double to, const audio::TempoMap &timing);

// The take clip with this id in the track's lanes (null for none), and its lane's index
const Clip *findTake(const Track &track, ObjectId take, size_t *lane = nullptr);
// The lane with this id (null for none)
const TakeLane *findLane(const Track &track, ObjectId lane);
TakeLane *findLane(Track &track, ObjectId lane);
// The lane a take comes from: its index among the track's lanes, or -1 (for an ordinary clip)
int laneOfSection(const Track &track, const Clip &section);
// "Take N" for a new lane: one more than the highest number the lanes' names have
std::string nextTakeName(const Track &track);

// The comp plays `take` over [from, to) (where the take has material): what was there is cut away
// (clips trimmed, split or removed), a section cut from the take goes in, and sections of one take
// that join up again are merged. The clips stay sorted.
bool compRange(Track &track, double from, double to, ObjectId take, const audio::TempoMap &timing);
// The range a click at `beat` switches: the clip of the comp under it, else the gap between its
// neighbours there
std::pair<double, double> sectionAround(const Track &track, double beat);
// The whole of a lane's takes in the comp
bool useLane(Track &track, ObjectId lane, const audio::TempoMap &timing);
// Each point of [from, to) from the newest take that covers it (the first lane first)
bool newestComp(Track &track, double from, double to, const audio::TempoMap &timing);
// The edge at `at` between two neighbouring clips of the comp moved to `to`: the section that grows
// takes more of its take (an ordinary clip can't grow), the other gives way. False when there is
// no such edge.
bool moveBoundary(Track &track, double at, double to, const audio::TempoMap &timing);
// The take above or below (by lane, `step` -1 or 1) for the comp section at `index`, over the same
// range; false when there is none with material there
bool stepTake(Track &track, size_t index, int step, const audio::TempoMap &timing);

// The ranges of a lane's takes that the comp plays, in order
std::vector<std::pair<double, double>> laneUse(const Track &track, ObjectId lane);
// The comp's edges between two neighbouring clips, in order (where comping can move them)
std::vector<double> compBoundaries(const Track &track);

// The take lanes go; the comp stays as ordinary clips
bool flattenComp(Track &track);
// Takes the comp doesn't use go (and lanes left empty)
bool deleteUnusedTakes(Track &track);
// A lane goes; what the comp cut from it stays, as ordinary clips
bool removeLane(Track &track, ObjectId lane);

// After a recording of `passes` (their clips, in the order they were played) onto the track, the
// passes no longer in its clips: when there was one pass over empty space it goes back as an
// ordinary clip (returns false). Otherwise each pass becomes a take in a new lane ("Take N", the
// newest first), the ordinary clips it was recorded over move into a lane of their own
// ("Original") so nothing is lost, and the comp over all of it is newestComp.
bool foldTakes(Track &track, std::vector<Clip> passes, const audio::TempoMap &timing);

}
