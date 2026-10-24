#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

// Warping: an audio clip whose sound follows the bars (docs/design/warp-plan.md). The project keeps
// a clip's Warp; the audio thread plays it through the map below, which turns the clip's own beats
// (its content beats) into the file's seconds.

namespace audio {

// How a warped clip is stretched: Repitch plays faster or slower as tape does (pitch with speed);
// Beats plays slices from the transients at the file's speed; Tones overlaps grains (WSOLA); Complex
// is a phase vocoder
enum class WarpMode : uint8_t { Repitch, Beats, Tones, Complex };
// Beats: where the sound is sliced
enum class WarpPreserve : uint8_t { Transients, Quarter, Eighth, Sixteenth };
// Beats: what a slice whose time is longer than its sound does at its end
enum class WarpLoop : uint8_t { Off, Forward, BackAndForth };

constexpr size_t kMaxWarpMarkers = 4096;
constexpr double kMinWarpBpm = 20.0;
constexpr double kMaxWarpBpm = 999.0;
constexpr double kMaxWarpPitch = 48.0; // semitones each way

// A moment of the file pinned to a beat of the clip's content
struct WarpMarker {
    double seconds = 0.0; // into the file
    double beat = 0.0;    // content beats

    bool operator==(const WarpMarker &o) const { return seconds == o.seconds && beat == o.beat; }
    bool operator!=(const WarpMarker &o) const { return !(*this == o); }
};

struct Warp {
    bool on = false;
    WarpMode mode = WarpMode::Complex;
    double bpm = 0.0; // the file's tempo in content beats a minute; 0 until the clip is first warped
    std::vector<WarpMarker> markers; // rising in both seconds and beats
    double pitch = 0.0;              // semitones (its fraction is the cents); Repitch ignores it
    WarpPreserve preserve = WarpPreserve::Transients;
    WarpLoop loop = WarpLoop::Off;
    float envelope = 100.0f; // Beats: each slice's decay, 0-100 (100: none)
    float grain = 50.0f;     // Tones: the grain, 0-100 (about 12 to 90 ms)

    // Whether the file is stretched by a voice (Beats, Tones, Complex) rather than read at a speed
    bool stretched() const { return on && mode != WarpMode::Repitch; }
    double secondsPerBeat() const { return 60.0 / (bpm > 0.0 ? bpm : 120.0); }

    bool operator==(const Warp &o) const
    {
        return on == o.on && mode == o.mode && bpm == o.bpm && markers == o.markers && pitch == o.pitch &&
               preserve == o.preserve && loop == o.loop && envelope == o.envelope && grain == o.grain;
    }
    bool operator!=(const Warp &o) const { return !(*this == o); }
};

// The map. Real-time (a binary search and a line). No markers: the clip's tempo from the file's
// start; one: that tempo through it; more: straight lines between them, the clip's tempo outside.
double warpSeconds(const Warp &warp, double beat);
double warpBeat(const Warp &warp, double seconds);
// Seconds of the file a content beat lasts at `beat`
double warpSecondsPerBeat(const Warp &warp, double beat);
// The segment `beat` is in: the index of the marker at or before it (-1 before the first)
int warpSegment(const Warp &warp, double beat);

// Puts a warp in order: values clamped, markers sorted by beat, those that don't rise in both seconds
// and beats dropped, at most kMaxWarpMarkers kept
void tidyWarp(Warp &warp);

// A Beats slice: where it starts in the file and on the clip's content beats
struct WarpSlice {
    double frame = 0.0; // file frames
    double beat = 0.0;  // content beats
};

}
