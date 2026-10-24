#pragma once

#include <string>
#include <vector>

namespace audio {

// How a clip's sound follows the song's tempo (docs/design/warping-plan.md)
enum class WarpMode {
    Off,     // at the file's own speed: the tempo doesn't stretch it
    Repitch, // faster or slower with the tempo, its pitch following its speed
    Beats,   // cut at its transients, each hit at its own speed and stretched between: drums
    Tonal,   // a phase vocoder: chords, voices, pads
};
const char *warpModeName(WarpMode mode); // "off", "repitch", "beats", "tonal" (the file's words)
const char *warpModeLabel(WarpMode mode); // "Off", "Re-pitch", "Beats", "Tonal"
bool warpModeFromName(const std::string &name, WarpMode &mode);

// A place in the file (seconds) pinned to a beat of the file's own count (its beat 0 is where the
// file's first beat would be)
struct WarpMarker {
    double seconds = 0.0;
    double beat = 0.0;

    bool operator==(const WarpMarker &o) const { return seconds == o.seconds && beat == o.beat; }
    bool operator!=(const WarpMarker &o) const { return !(*this == o); }
};

// A clip's warp: its mode, the file's tempo (outside the markers), a transpose in semitones
// (fractions are cents; Beats and Tonal only) and the markers, sorted with both fields increasing
struct ClipWarp {
    static constexpr double kMinBpm = 20.0, kMaxBpm = 999.0;
    static constexpr float kMaxTranspose = 24.0f;

    WarpMode mode = WarpMode::Off;
    double bpm = 120.0;
    float transpose = 0.0f;
    std::vector<WarpMarker> markers;

    bool on() const { return mode != WarpMode::Off; }
    // The pitch ratio the transpose asks for (1 for Off and Repitch, which don't transpose)
    double pitch() const;

    bool operator==(const ClipWarp &o) const
    {
        return mode == o.mode && bpm == o.bpm && transpose == o.transpose && markers == o.markers;
    }
    bool operator!=(const ClipWarp &o) const { return !(*this == o); }
};

// The file's beat at a place in it, and the place at a beat: straight lines between the markers,
// the warp's bpm before the first and after the last (and throughout with none). Any number in, so
// places before the file's start or after its end extend the same way.
double warpBeatAt(const ClipWarp &warp, double seconds);
double warpSecondsAt(const ClipWarp &warp, double beat);
// The file's tempo at a place in it (its marker segment's, or the bpm)
double warpBpmAt(const ClipWarp &warp, double seconds);
// Clamps the bpm and transpose, sorts the markers by seconds and drops any whose beat doesn't rise
// (or that sits on the same place as the one before)
void tidyWarp(ClipWarp &warp);

}
