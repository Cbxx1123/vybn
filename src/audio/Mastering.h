#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

// Mastering tools for exports. They run on background threads over chunks of a render, never on
// the audio thread, so their constructors may allocate; add and process don't allocate beyond
// the growth of the loudness history. Each class says what its caller must do.
//
//   LoudnessMeter   EBU R128 loudness: integrated (gated), momentary, short-term, loudness range
//   TruePeakMeter   the largest level between samples (4x oversampled), and samplePeak
//   TruePeakLimiter holds the true peak under a ceiling with a 5 ms lookahead, aligned to its input
//   Dither          reduces a render to 16 or 24 bits, with or without noise shaping
//   applyFade       equal-power fades in and out of a stream

namespace audio::master {

// The two K-weighting biquads of ITU-R BS.1770, as direct coefficients (y = b0 x + ..., with the
// feedback a1 and a2). At 48 kHz they are the published values; other rates are derived from the
// standard's analogue prototypes with the bilinear transform.
struct KWeightCoefficients {
    double b0, b1, b2, a1, a2;
};

void kWeightingCoefficients(double sampleRate, KWeightCoefficients &shelf, KWeightCoefficients &highPass);

// EBU R128 loudness of an interleaved stream. Each channel is K-weighted and its mean square is
// summed with the others (weight 1 each, mono and stereo). The stream is cut into 100 ms chunks,
// and a window of chunks has the loudness -0.691 + 10 log10(mean of its chunk sums). Momentary
// is 400 ms, short-term 3 s, and integrated uses 400 ms blocks every 100 ms with the absolute
// gate at -70 LUFS and the relative gate 10 LU under the gated mean. Silence reads as negative
// infinity. Memory grows by one double per 100 ms. A partial chunk at the end isn't counted until
// it is complete. Feed it from one thread, in any block sizes.
class LoudnessMeter {
public:
    LoudnessMeter(uint32_t channels, double sampleRate);

    void reset();
    void add(const float *interleaved, size_t frames);

    // LUFS over the last 400 ms (negative infinity before that much audio)
    double momentary() const;
    // LUFS over the last 3 s
    double shortTerm() const;
    // The loudest short-term value so far
    double shortTermMax() const;
    // Gated programme loudness so far, in LUFS
    double integrated() const;
    // Loudness range in LU (EBU Tech 3342): the 95th minus the 10th percentile of the short-term
    // values above the absolute gate and 20 LU under their mean. Zero when nothing passes.
    double range() const;
    // Short-term values in LUFS at the end of each interval (rounded to whole 100 ms chunks),
    // from the first complete 3 s window on
    std::vector<double> shortTermSeries(double intervalSeconds) const;

private:
    uint32_t channels_;
    double sampleRate_;
    size_t chunkFrames_;
    KWeightCoefficients shelf_{}, highPass_{};
    std::vector<double> state_; // per channel: the shelf's two delays, then the high-pass's
    double chunkSum_ = 0.0;     // the current chunk's sum of squares, over channels
    size_t chunkFill_ = 0;
    std::vector<double> chunks_; // each finished chunk's sum of mean squares, over channels
};

// The largest absolute sample of an interleaved block: linear, 1 is full scale.
double samplePeak(const float *interleaved, size_t frames, uint32_t channels);

// The true peak: the largest level the signal reaches between its samples, found on a 4x
// oversampled copy (a 48-tap windowed-sinc interpolator with its cutoff at 0.45 of the sample
// rate). Feed it from one thread; reset clears the peak.
class TruePeakMeter {
public:
    explicit TruePeakMeter(uint32_t channels);

    void reset();
    void add(const float *interleaved, size_t frames);

    // Linear, the highest level since the last reset (zero when nothing was added)
    double peak() const;
    // The same in dBTP
    double peakDb() const;

private:
    uint32_t channels_;
    std::vector<double> ring_; // per channel, the last 16 input samples
    uint64_t count_ = 0;       // input frames added
    double peak_ = 0.0;
};

// A true-peak limiter. The gain holds the oversampled peaks under the ceiling (in dBTP), linked
// across the channels. Each frame needs the gain its own peak asks for; the gain applied to a
// frame is the moving average over the lookahead (5 ms) of the least requirement in each window
// that covers it, so the reduction starts smoothly before a peak and never overshoots. It recovers
// toward unity at the release rate, which is 50 to 200 ms with the depth of the reduction when
// releaseMs is 0 (auto), else releaseMs.
//
// The output lags the input by latency() frames. process() writes the output for the frames
// that have come through, so it returns at most as many frames as it was given. finish() writes
// the rest after the last input, so over a whole stream the output has exactly as many frames as
// the input, and output frame n belongs to input frame n. Output may be written over the input
// (the same buffer). reset() starts a new stream.
class TruePeakLimiter {
public:
    TruePeakLimiter(uint32_t channels, double sampleRate, double ceilingDb = -1.0, double releaseMs = 0.0);

    void reset();
    size_t latency() const;

    // Interleaved frames in, up to `frames` interleaved frames out; returns the frames written.
    size_t process(const float *in, float *out, size_t frames);
    // Writes the last latency() frames (fewer when the stream was shorter), returns the count.
    size_t finish(float *out);

private:
    struct Entry {
        uint64_t frame;
        double required;
    };

    // Takes one input frame (or silence when `in` is null); true when an output frame was written
    bool push(const float *in, float *out);

    uint32_t channels_;
    size_t lookahead_;  // frames the gain looks ahead over
    size_t delay_;      // the latency: the lookahead and the interpolator's reach
    double ceiling_;    // linear
    double releaseFast_, releaseSlow_; // the rise per frame, as fractions of the way back to unity
    bool autoRelease_;
    uint64_t mask_;     // the rings' size minus one
    std::vector<double> ring_;     // per channel, the input frames still needed
    std::vector<double> smoothed_; // per frame, the least requirement over the lookahead
    std::vector<Entry> queue_;     // requirements in increasing order of frame (a monotone queue)
    uint64_t queueHead_ = 0, queueTail_ = 0;
    uint64_t count_ = 0; // input frames pushed
    double sum_ = 0.0;   // the lookahead's sum of smoothed_
    double gain_ = 1.0;
};

// Dither: the noise and rounding that reduce a float render to a bit depth. Tpdf adds triangular
// noise of one LSB peak before rounding. NoiseShaped also feeds the rounding error back through a
// second-order filter, so the error moves toward high frequencies. None only rounds. The random
// numbers come from a generator seeded by the caller, so renders repeat. Per-channel state carries
// across calls.
enum class DitherMode { None, Tpdf, NoiseShaped };

class Dither {
public:
    Dither(DitherMode mode, uint32_t channels, uint32_t seed = 1);

    void reset();
    // Quantizes interleaved samples in place to `bits` (16 or 24). 32 or more leaves them alone.
    void apply(float *interleaved, size_t frames, uint32_t channels, int bits);

private:
    double uniform();

    DitherMode mode_;
    uint64_t state_;
    std::vector<double> error1_, error2_; // per channel: the last two rounding errors
};

// Applies equal-power fades to a block of an interleaved stream. The fade in runs from the start
// of the file over fadeInFrames, the fade out ends at totalFrames over fadeOutFrames (zero means
// no fade). `position` is the index of the block's first frame in the file.
void applyFade(float *interleaved, size_t frames, uint32_t channels, size_t fadeInFrames, size_t fadeOutFrames,
               size_t totalFrames, size_t position);

}
