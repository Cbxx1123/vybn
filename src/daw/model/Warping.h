#pragma once

#include <string>
#include <vector>

#include "daw/model/Project.h"
#include "audio/Stretch.h"
#include "audio/Warp.h"

namespace daw {

// Warping audio clips (docs/design/warping-plan.md): the edits, each keeping what is under the clip,
// and how a clip's stretched sound is placed. A warped clip plays file beat
// warpBeatAt(offset) + (b - start) at timeline beat b; its anchor is where the file's beat 0 lands.

// Warping on (any mode but Off) or off. Turned on from Off, the file's tempo is `bpm` (the caller
// passes the song's tempo at the clip, or a detected one) and its markers are kept; either way the
// clip covers the same audio as before, its length in beats following.
void setWarpMode(Clip &clip, const audio::TempoMap &timing, audio::WarpMode mode, double bpm);
// A new tempo for the file: the clip's start keeps its audio and its length follows what it covered
void setWarpBpm(Clip &clip, double bpm);
// New markers (tidied): the file beat at the clip's start is kept, so only the audio around a moved
// marker stretches; the length in beats stays
void setWarpMarkers(Clip &clip, std::vector<audio::WarpMarker> markers);
// The timeline beat of the file's beat 0
double warpAnchor(const Clip &clip);
// A marker at each transient inside the clip, its beat moved onto the nearest line of a `grid`
// (beats) on the timeline, `strength` of the way (0-1). Markers that would cross are dropped.
std::vector<audio::WarpMarker> snapTransients(const audio::TempoMap &timing, const Clip &clip,
                                              const std::vector<double> &transients, double grid, double strength = 1.0);
// The file's tempo the clip's length makes it, if the clip's audio (offset to its end) is `bars`
// bars of the song's signature at the clip
double tempoForBars(const audio::TempoMap &timing, const Clip &clip, int bars);

// A warped clip's stretched sound covers the whole file, laid at the clip's place: output time 0 is
// the file's start (file beat warpBeatAt(0) at the anchor). With one tempo it doesn't depend on
// where the clip is, so the anchor that keys it is fixed (`renderAnchor`).
double renderAnchor(const audio::TempoMap &timing, const Clip &clip);
// The path the render follows (output seconds to file seconds), for a file `fileSeconds` long
audio::TimePath warpPath(const audio::TempoMap &timing, const audio::ClipWarp &warp, double anchor, double fileSeconds);
// What makes a render different from another of the same file: the warp, and the tempo (its BPM,
// or the map and the anchor when the tempo changes)
std::string warpKey(const audio::TempoMap &timing, const audio::ClipWarp &warp, double anchor);
// Seconds into a render made with `renderWarp` where `clip` starts
double warpRenderOffset(const audio::TempoMap &timing, const Clip &clip, const audio::ClipWarp &renderWarp);

}
