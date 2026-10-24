#pragma once

#include <string>

#include "audio/AudioAnalysis.h"
#include "audio/TempoMap.h"
#include "audio/Warp.h"
#include "daw/model/Project.h"

// Warping in the project (docs/design/warp-plan.md): an audio clip's `warp` and the rules that keep
// its sound in place as it is warped, trimmed, split and stretched. A clip's `offset` is the file's
// seconds at its start, warped or not; a warped clip's content beat there is warpBeat(warp, offset),
// and beat b of the timeline plays the file at warpSeconds(warp, contentStart + b - start).

namespace daw {

// The file's words and the menus' names
const char *warpModeKey(audio::WarpMode mode);
const char *warpModeName(audio::WarpMode mode);
bool warpModeFromKey(const std::string &key, audio::WarpMode &mode);
const char *warpPreserveKey(audio::WarpPreserve preserve); // "transients", "1/4", "1/8", "1/16"
const char *warpPreserveName(audio::WarpPreserve preserve);
bool warpPreserveFromKey(const std::string &key, audio::WarpPreserve &preserve);
const char *warpLoopKey(audio::WarpLoop loop); // "off", "forward", "back and forth"
const char *warpLoopName(audio::WarpLoop loop);
bool warpLoopFromKey(const std::string &key, audio::WarpLoop &loop);
// The slices Preserve makes in content beats (0 for Transients)
double warpPreserveBeats(audio::WarpPreserve preserve);

// The content beat at a warped clip's start
double clipContentStart(const Clip &clip);
// The file's seconds playing at a timeline beat, and the timeline beat a moment of the file plays at
double clipSourceSeconds(const Clip &clip, const audio::TempoMap &timing, double beat);
double clipBeatAt(const Clip &clip, const audio::TempoMap &timing, double seconds);
// Where the file's start falls on the timeline, and its end (a file `fileSeconds` long)
double fileStartBeat(const Clip &clip, const audio::TempoMap &timing);
double fileEndBeat(const Clip &clip, const audio::TempoMap &timing, double fileSeconds);
// The clip's tempo where its content plays at `beat` of the timeline (its markers' stretch included)
double clipTempoAt(const Clip &clip, double beat);

// A warp that plays the clip exactly where it plays unwarped under `timing`: the tempo at its start,
// and markers where the tempo changes over the file (every beat through a ramp)
audio::Warp followTiming(const Clip &clip, const audio::TempoMap &timing, double fileSeconds);
// Turns warp on or off, keeping the clip's start and its file's moment there. On: the first time its
// sound stays where it is (followTiming); again, the map it had. Off: its length cut to what its file
// lasts. `fileSeconds` is the file's length (0 when not known: nothing is cut).
void setWarp(Clip &clip, const audio::TempoMap &timing, bool on, double fileSeconds);
// The clip's tempo set: its markers' beats scaled with it (about content beat 0) and its length too,
// so it holds the same sound
void setClipTempo(Clip &clip, double bpm);
// Stretches a warped clip's content to fill `beats` (Alt-dragging its end)
void fitToLength(Clip &clip, double beats);

// Markers; each returns the index it ended at (-1: none). Added at a timeline beat where the file
// plays there now, so nothing moves
int addWarpMarker(Clip &clip, double beat);
// A marker moved to a timeline beat, between its neighbours: the sound either side stretches. The clip's
// start is anchored first (anchorClipStart), and the anchor itself doesn't move.
int moveWarpMarker(Clip &clip, int index, double beat);
// The sound under a marker slipped to another moment of the file (the beat staying)
int slipWarpMarker(Clip &clip, int index, double seconds);
void removeWarpMarker(Clip &clip, int index);
// All markers gone but one at the clip's start, so it keeps its place at the clip's tempo
void clearWarpMarkers(Clip &clip);
// The markers after `index` gone: the clip's tempo from there on
void warpStraightFrom(Clip &clip, int index);
// The marker nearest a content beat within `within` beats, else -1
int warpMarkerNear(const Clip &clip, double contentBeat, double within);
// A marker at the clip's start (the file's moment there on its beat), so moving others doesn't move it
int anchorClipStart(Clip &clip);
// Each marker but the start's moved toward the nearest timeline grid line by `strength` (0-1)
void quantiseWarpMarkers(Clip &clip, double grid, double strength);
// Audio quantise: a marker at each transient of the clip's sound at or above `threshold`, then quantised
// as above. Turns warp on first when it is off. Returns the markers made.
int quantiseAudio(Clip &clip, const audio::TempoMap &timing, const audio::AudioAnalysis &analysis, double grid,
                  double strength, float threshold);

// Imports look for loops only in files this short (32 bars at 70 BPM is under two minutes)
constexpr double kLongestLoopSeconds = 120.0;

// What the analysis suggests: Beats for percussive sound, else Complex
audio::WarpMode guessMode(const audio::AudioAnalysis &analysis);
// The clip warped at the tempo found (its first downbeat on the beat its start would put it, the clip
// starting earlier for a pickup when there is room), in the guessed mode, as long as its file lasts.
// False, and nothing changed, when the analysis found no tempo.
bool applyGuess(Clip &clip, const audio::AudioAnalysis &analysis);
// Only the tempo found applied to a clip already placed (its start and the file's moment there stay)
bool applyGuessedTempo(Clip &clip, const audio::TempoMap &timing, const audio::AudioAnalysis &analysis);

// Beats' slices for a clip: at its transients (above a floor) or every `warpPreserveBeats`, as file
// frames and content beats, with one at the file's start
std::vector<audio::WarpSlice> warpSlices(const Clip &clip, const audio::AudioAnalysis *analysis, double fileRate,
                                         uint64_t fileFrames);

// The words a clip's warp is shown with: "Complex", "Beats +3 st"
std::string warpBadge(const Clip &clip);

}
