#pragma once

#include <atomic>
#include <cstdint>
#include <vector>

#include "Warp.h"

namespace audio {

// Where an output plays from in its source: the source's seconds at each moment of the output,
// sampled every `step` seconds and joined by straight lines (extended at the ends by their last
// slope). It only rises, so it can be turned round (`outputAt`).
struct TimePath {
    double step = 0.005;
    std::vector<double> source;

    double at(double seconds) const;
    // The output's seconds where the source reaches `seconds`
    double outputAt(double seconds) const;
    double duration() const { return source.size() > 1 ? static_cast<double>(source.size() - 1) * step : 0.0; }
};

// Interleaved audio to stretch (read only)
struct StretchSource {
    const float *samples = nullptr;
    uint64_t frames = 0;
    uint32_t channels = 1;
    double sampleRate = 48000.0;
};

// Offline time-stretching and pitch-shifting, for a background thread (docs/design/warping-plan.md).
// Renders `path.duration()` seconds of output at the source's rate and channels, interleaved, where
// output time t plays the source at path.at(t):
//   Repitch  four-point interpolation along the path (its pitch follows its speed; `pitch` unused)
//   Tonal    a phase vocoder with identity phase locking, the stereo image kept and the phases
//            reset at the transients
//   Beats    the source cut at its transients, each piece starting where its transient lands and
//            stretched by WSOLA, its attack as recorded
// For Tonal and Beats, `pitch` (a ratio) transposes without changing the timing. `transients` are
// source seconds, rising. Off renders as Repitch. Returns empty when cancelled (checked often).
std::vector<float> stretchAudio(const StretchSource &source, const TimePath &path, WarpMode mode, double pitch,
                                const std::vector<double> &transients, const std::atomic<bool> *cancel = nullptr);

}
