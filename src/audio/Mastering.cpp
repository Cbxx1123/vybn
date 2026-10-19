#include "Mastering.h"

#include <algorithm>
#include <cmath>
#include <limits>

#include "AudioProcessor.h"

namespace audio::master {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kInfinity = std::numeric_limits<double>::infinity();

// Loudness works in chunks of 100 ms: a 400 ms block is four chunks and advances by one, so
// consecutive blocks overlap by 75%. The short-term window is 3 s, thirty chunks.
constexpr double kChunkSeconds = 0.1;
constexpr size_t kBlockChunks = 4;
constexpr size_t kShortChunks = 30;
constexpr double kAbsoluteGate = -70.0;
constexpr double kIntegratedRelativeGate = -10.0;
constexpr double kRangeRelativeGate = -20.0;

// Loudness of a mean square (energy), and the energy of a loudness
double loudnessOf(double energy)
{
    return energy > 0.0 ? -0.691 + 10.0 * std::log10(energy) : -kInfinity;
}

double energyOf(double loudness)
{
    return std::pow(10.0, (loudness + 0.691) / 10.0);
}

// The mean of the `count` chunks ending at `end`
double windowEnergy(const std::vector<double> &chunks, size_t end, size_t count)
{
    double sum = 0.0;
    for (size_t i = end + 1 - count; i <= end; ++i)
        sum += chunks[i];
    return sum / static_cast<double>(count);
}

// One sample through a biquad in transposed direct form II; `state` holds its two delays
inline double biquad(const KWeightCoefficients &c, double *state, double x)
{
    const double y = c.b0 * x + state[0];
    state[0] = c.b1 * x - c.a1 * y + state[1];
    state[1] = c.b2 * x - c.a2 * y;
    return y;
}

// A percentile of sorted values, interpolating between neighbours
double percentile(const std::vector<double> &sorted, double p)
{
    const double at = p * static_cast<double>(sorted.size() - 1);
    const size_t low = static_cast<size_t>(at);
    const size_t high = std::min(low + 1, sorted.size() - 1);
    const double t = at - static_cast<double>(low);
    return sorted[low] * (1.0 - t) + sorted[high] * t;
}

// The interpolator: phase p is the value a quarter sample further on than phase p - 1, computed as
// the sum of taps[p][k] times the sample k frames back (12 taps a phase, 48 in all). Each phase
// has unit gain at DC.
struct Interpolator {
    double taps[4][12];
};

const Interpolator &interpolator()
{
    static const Interpolator table = [] {
        Interpolator t{};
        const double cutoff = 0.45; // in cycles per input sample
        double prototype[48];
        for (int m = 0; m < 48; ++m) {
            // Blackman window over the 48 taps, and the windowed sinc at each tap's time (quarter
            // samples, from the centre at tap 24)
            const double x = (m - 24) / 24.0;
            const double window = 0.42 + 0.5 * std::cos(kPi * x) + 0.08 * std::cos(2.0 * kPi * x);
            const double arg = 2.0 * cutoff * (m - 24) / 4.0;
            const double sinc = std::fabs(arg) < 1e-12 ? 1.0 : std::sin(kPi * arg) / (kPi * arg);
            prototype[m] = 2.0 * cutoff * sinc * window;
        }
        for (int p = 0; p < 4; ++p) {
            double sum = 0.0;
            for (int k = 0; k < 12; ++k)
                sum += prototype[4 * k + p];
            for (int k = 0; k < 12; ++k)
                t.taps[p][k] = prototype[4 * k + p] / sum;
        }
        return t;
    }();
    return table;
}

// The four oversampled points between frame newest - 6 and frame newest - 5, read from a ring
// indexed by frame number (mask is its size minus one)
void interpolate(const double *ring, uint64_t mask, uint64_t newest, double out[4])
{
    const Interpolator &t = interpolator();
    for (int p = 0; p < 4; ++p) {
        double sum = 0.0;
        for (uint64_t k = 0; k < 12; ++k)
            sum += t.taps[p][k] * ring[(newest - k) & mask];
        out[p] = sum;
    }
}

constexpr uint64_t kMeterMask = 15;   // the true peak meter's ring: 16 frames a channel
constexpr size_t kInterpolatorReach = 6;

} // namespace

void kWeightingCoefficients(double sampleRate, KWeightCoefficients &shelf, KWeightCoefficients &highPass)
{
    // The pre-filter, a high shelf: the analogue prototype's centre, gain and Q, through the
    // bilinear transform
    const double f0 = 1681.9744509555319;
    const double gainDb = 3.99984385397;
    const double q = 0.7071752369554193;
    const double k = std::tan(kPi * f0 / sampleRate);
    const double vh = std::pow(10.0, gainDb / 20.0);
    const double vb = std::pow(vh, 0.4996667741545416);
    const double a0 = 1.0 + k / q + k * k;
    shelf.b0 = (vh + vb * k / q + k * k) / a0;
    shelf.b1 = 2.0 * (k * k - vh) / a0;
    shelf.b2 = (vh - vb * k / q + k * k) / a0;
    shelf.a1 = 2.0 * (k * k - 1.0) / a0;
    shelf.a2 = (1.0 - k / q + k * k) / a0;

    // The RLB high-pass. The standard's numerator is 1, -2, 1 (not divided by the denominator's
    // leading term), so its published coefficients are reproduced exactly at 48 kHz
    const double f1 = 38.13547087602444;
    const double q1 = 0.5003270373238773;
    const double k1 = std::tan(kPi * f1 / sampleRate);
    const double d0 = 1.0 + k1 / q1 + k1 * k1;
    highPass.b0 = 1.0;
    highPass.b1 = -2.0;
    highPass.b2 = 1.0;
    highPass.a1 = 2.0 * (k1 * k1 - 1.0) / d0;
    highPass.a2 = (1.0 - k1 / q1 + k1 * k1) / d0;
}

// LoudnessMeter

LoudnessMeter::LoudnessMeter(uint32_t channels, double sampleRate)
    : channels_(std::max<uint32_t>(1, channels)), sampleRate_(sampleRate)
{
    chunkFrames_ = std::max<size_t>(1, static_cast<size_t>(std::lround(sampleRate * kChunkSeconds)));
    kWeightingCoefficients(sampleRate, shelf_, highPass_);
    state_.assign(channels_ * 4, 0.0);
}

void LoudnessMeter::reset()
{
    std::fill(state_.begin(), state_.end(), 0.0);
    chunks_.clear();
    chunkSum_ = 0.0;
    chunkFill_ = 0;
}

void LoudnessMeter::add(const float *interleaved, size_t frames)
{
    for (size_t f = 0; f < frames; ++f) {
        for (uint32_t ch = 0; ch < channels_; ++ch) {
            double *state = &state_[ch * 4];
            const double y = biquad(highPass_, state + 2, biquad(shelf_, state, interleaved[f * channels_ + ch]));
            chunkSum_ += y * y;
        }
        if (++chunkFill_ == chunkFrames_) {
            chunks_.push_back(chunkSum_ / static_cast<double>(chunkFrames_));
            chunkSum_ = 0.0;
            chunkFill_ = 0;
        }
    }
}

double LoudnessMeter::momentary() const
{
    if (chunks_.size() < kBlockChunks)
        return -kInfinity;
    return loudnessOf(windowEnergy(chunks_, chunks_.size() - 1, kBlockChunks));
}

double LoudnessMeter::shortTerm() const
{
    if (chunks_.size() < kShortChunks)
        return -kInfinity;
    return loudnessOf(windowEnergy(chunks_, chunks_.size() - 1, kShortChunks));
}

double LoudnessMeter::shortTermMax() const
{
    double loudest = -kInfinity;
    for (size_t end = kShortChunks - 1; end < chunks_.size(); ++end)
        loudest = std::max(loudest, loudnessOf(windowEnergy(chunks_, end, kShortChunks)));
    return loudest;
}

double LoudnessMeter::integrated() const
{
    // The absolute gate, then the relative gate 10 LU under the mean of what passed it
    const double absolute = energyOf(kAbsoluteGate);
    double sum = 0.0;
    size_t count = 0;
    for (size_t end = kBlockChunks - 1; end < chunks_.size(); ++end) {
        const double z = windowEnergy(chunks_, end, kBlockChunks);
        if (z > absolute) {
            sum += z;
            ++count;
        }
    }
    if (count == 0)
        return -kInfinity;
    const double relative = energyOf(loudnessOf(sum / static_cast<double>(count)) + kIntegratedRelativeGate);
    double gated = 0.0;
    size_t kept = 0;
    for (size_t end = kBlockChunks - 1; end < chunks_.size(); ++end) {
        const double z = windowEnergy(chunks_, end, kBlockChunks);
        if (z > absolute && z > relative) {
            gated += z;
            ++kept;
        }
    }
    return kept > 0 ? loudnessOf(gated / static_cast<double>(kept)) : -kInfinity;
}

double LoudnessMeter::range() const
{
    // The short-term values above the absolute gate, then those above 20 LU under their mean
    const double absolute = energyOf(kAbsoluteGate);
    std::vector<double> values;
    double energySum = 0.0;
    for (size_t end = kShortChunks - 1; end < chunks_.size(); ++end) {
        const double energy = windowEnergy(chunks_, end, kShortChunks);
        if (energy > absolute) {
            values.push_back(loudnessOf(energy));
            energySum += energy;
        }
    }
    if (values.empty())
        return 0.0;
    const double relative = loudnessOf(energySum / static_cast<double>(values.size())) + kRangeRelativeGate;
    std::vector<double> kept;
    for (double value : values)
        if (value > relative)
            kept.push_back(value);
    if (kept.empty())
        return 0.0;
    std::sort(kept.begin(), kept.end());
    return percentile(kept, 0.95) - percentile(kept, 0.10);
}

std::vector<double> LoudnessMeter::shortTermSeries(double intervalSeconds) const
{
    const double chunkSeconds = static_cast<double>(chunkFrames_) / sampleRate_;
    const size_t step = std::max<size_t>(1, static_cast<size_t>(std::lround(intervalSeconds / chunkSeconds)));
    std::vector<double> series;
    for (size_t end = kShortChunks - 1; end < chunks_.size(); ++end)
        if ((end + 1) % step == 0)
            series.push_back(loudnessOf(windowEnergy(chunks_, end, kShortChunks)));
    return series;
}

double samplePeak(const float *interleaved, size_t frames, uint32_t channels)
{
    double peak = 0.0;
    const size_t count = frames * channels;
    for (size_t i = 0; i < count; ++i)
        peak = std::max(peak, std::fabs(static_cast<double>(interleaved[i])));
    return peak;
}

// TruePeakMeter

TruePeakMeter::TruePeakMeter(uint32_t channels) : channels_(std::max<uint32_t>(1, channels))
{
    ring_.assign(channels_ * (kMeterMask + 1), 0.0);
}

void TruePeakMeter::reset()
{
    std::fill(ring_.begin(), ring_.end(), 0.0);
    count_ = 0;
    peak_ = 0.0;
}

void TruePeakMeter::add(const float *interleaved, size_t frames)
{
    for (size_t f = 0; f < frames; ++f) {
        const uint64_t newest = count_++;
        for (uint32_t ch = 0; ch < channels_; ++ch)
            ring_[ch * (kMeterMask + 1) + (newest & kMeterMask)] = interleaved[f * channels_ + ch];
        for (uint32_t ch = 0; ch < channels_; ++ch) {
            double points[4];
            interpolate(&ring_[ch * (kMeterMask + 1)], kMeterMask, newest, points);
            for (double point : points)
                peak_ = std::max(peak_, std::fabs(point));
        }
    }
}

double TruePeakMeter::peak() const
{
    return peak_;
}

double TruePeakMeter::peakDb() const
{
    return peak_ > 0.0 ? 20.0 * std::log10(peak_) : -kInfinity;
}

// TruePeakLimiter

TruePeakLimiter::TruePeakLimiter(uint32_t channels, double sampleRate, double ceilingDb, double releaseMs)
    : channels_(std::max<uint32_t>(1, channels))
{
    lookahead_ = std::max<size_t>(1, static_cast<size_t>(std::lround(sampleRate * 0.005)));
    delay_ = lookahead_ + kInterpolatorReach - 1;
    ceiling_ = std::pow(10.0, ceilingDb / 20.0);

    // The rings hold the delay, the interpolator's reach and the lookahead, rounded to a power of two
    size_t size = 64;
    while (size < delay_ + 2 * kInterpolatorReach + lookahead_)
        size *= 2;
    mask_ = size - 1;
    ring_.assign(channels_ * size, 0.0);
    smoothed_.assign(size, 0.0);
    queue_.assign(size, Entry{0, 0.0});
    sum_ = 0.0;

    // A release is a time constant: each frame the gain closes a fraction of the way back to unity.
    // The auto release blends between 50 ms (a shallow reduction) and 200 ms (a deep one).
    autoRelease_ = releaseMs <= 0.0;
    releaseFast_ = 1.0 - std::exp(-1.0 / ((autoRelease_ ? 0.05 : releaseMs * 0.001) * sampleRate));
    releaseSlow_ = autoRelease_ ? 1.0 - std::exp(-1.0 / (0.2 * sampleRate)) : releaseFast_;
}

void TruePeakLimiter::reset()
{
    std::fill(ring_.begin(), ring_.end(), 0.0);
    std::fill(smoothed_.begin(), smoothed_.end(), 0.0);
    queueHead_ = queueTail_ = 0;
    count_ = 0;
    sum_ = 0.0;
    gain_ = 1.0;
}

size_t TruePeakLimiter::latency() const
{
    return delay_;
}

size_t TruePeakLimiter::process(const float *in, float *out, size_t frames)
{
    size_t written = 0;
    for (size_t f = 0; f < frames; ++f)
        if (push(in + f * channels_, out + written * channels_))
            ++written;
    return written;
}

size_t TruePeakLimiter::finish(float *out)
{
    // Silence after the stream carries the last real frames out through the lookahead
    size_t written = 0;
    for (size_t i = 0; i < delay_; ++i)
        if (push(nullptr, out + written * channels_))
            ++written;
    return written;
}

bool TruePeakLimiter::push(const float *in, float *out)
{
    const uint64_t n = count_++;
    const uint64_t size = mask_ + 1;
    for (uint32_t ch = 0; ch < channels_; ++ch)
        ring_[ch * size + (n & mask_)] = in ? static_cast<double>(in[ch]) : 0.0;
    if (n < kInterpolatorReach)
        return false;

    // The oversampled peak between frame n - 6 and n - 5 (every channel): the requirement of frame n - 6
    const uint64_t frame = n - kInterpolatorReach;
    double peak = 0.0;
    for (uint32_t ch = 0; ch < channels_; ++ch) {
        double points[4];
        interpolate(&ring_[ch * size], mask_, n, points);
        for (double point : points)
            peak = std::max(peak, std::fabs(point));
    }
    const double required = peak > ceiling_ ? ceiling_ / peak : 1.0;

    // A monotone queue of requirements: its front is the least over the frames still in the window
    while (queueTail_ > queueHead_ && queue_[(queueTail_ - 1) & mask_].required >= required)
        --queueTail_;
    queue_[queueTail_ & mask_] = Entry{frame, required};
    ++queueTail_;

    // The window of lookahead frames ends at this frame, so it starts `lookahead - 1` frames back
    if (frame + 1 < lookahead_)
        return false;
    const uint64_t first = frame + 1 - lookahead_;
    while (queue_[queueHead_ & mask_].frame < first)
        ++queueHead_;
    smoothed_[first & mask_] = queue_[queueHead_ & mask_].required;

    // Moving average over the lookahead of the smoothed requirements. Before the stream starts the
    // window holds copies of the first value, which covers the first frames' requirements too, so
    // the gain never rises above any of them
    if (first == 0)
        sum_ = static_cast<double>(lookahead_) * smoothed_[0];
    else
        sum_ += smoothed_[first & mask_] - (first >= lookahead_ ? smoothed_[(first - lookahead_) & mask_] : smoothed_[0]);
    const double target = sum_ / static_cast<double>(lookahead_);

    // Falls at once to the target, rises toward unity at the release rate
    const double depth = 1.0 - gain_;
    const double fraction = autoRelease_ ? releaseFast_ + (releaseSlow_ - releaseFast_) * std::clamp(depth * 2.0, 0.0, 1.0)
                                         : releaseFast_;
    gain_ = std::min(target, gain_ + depth * fraction);
    for (uint32_t ch = 0; ch < channels_; ++ch)
        out[ch] = static_cast<float>(ring_[ch * size + (first & mask_)] * gain_);
    return true;
}

// Dither

Dither::Dither(DitherMode mode, uint32_t channels, uint32_t seed) : mode_(mode)
{
    state_ = (static_cast<uint64_t>(seed) * 0x9E3779B97F4A7C15ull) | 1ull;
    error1_.assign(std::max<uint32_t>(1, channels), 0.0);
    error2_.assign(std::max<uint32_t>(1, channels), 0.0);
}

void Dither::reset()
{
    std::fill(error1_.begin(), error1_.end(), 0.0);
    std::fill(error2_.begin(), error2_.end(), 0.0);
}

double Dither::uniform()
{
    // xorshift64: a value in [0, 1)
    state_ ^= state_ << 13;
    state_ ^= state_ >> 7;
    state_ ^= state_ << 17;
    return static_cast<double>(state_ >> 11) * (1.0 / 9007199254740992.0);
}

void Dither::apply(float *interleaved, size_t frames, uint32_t channels, int bits)
{
    if (bits >= 32 || bits < 2 || channels == 0)
        return;
    if (error1_.size() < channels) {
        error1_.assign(channels, 0.0);
        error2_.assign(channels, 0.0);
    }
    // The grid: whole steps of 2^(bits - 1) per unit of full scale
    const double scale = std::ldexp(1.0, bits - 1);
    const double low = -scale;
    const double high = scale - 1.0;
    // The noise shaping filter: the rounding error is fed back with these two coefficients
    const double c1 = 1.623;
    const double c2 = -0.982;
    for (size_t f = 0; f < frames; ++f) {
        for (uint32_t ch = 0; ch < channels; ++ch) {
            float &sample = interleaved[f * channels + ch];
            double &e1 = error1_[ch];
            double &e2 = error2_[ch];
            double value = static_cast<double>(sample) * scale;
            if (mode_ == DitherMode::NoiseShaped)
                value -= c1 * e1 + c2 * e2;
            if (mode_ != DitherMode::None)
                value += uniform() + uniform() - 1.0; // triangular, one LSB peak
            const double rounded = std::clamp(std::round(value), low, high);
            if (mode_ == DitherMode::NoiseShaped) {
                e2 = e1;
                e1 = std::clamp(rounded - value, -1.0, 1.0);
            }
            sample = static_cast<float>(rounded / scale);
        }
    }
}

// Fades

void applyFade(float *interleaved, size_t frames, uint32_t channels, size_t fadeInFrames, size_t fadeOutFrames,
               size_t totalFrames, size_t position)
{
    for (size_t f = 0; f < frames; ++f) {
        const size_t index = position + f;
        double gain = 1.0;
        if (fadeInFrames > 0 && index < fadeInFrames)
            gain *= std::sin(kPi / 2.0 * static_cast<double>(index) / static_cast<double>(fadeInFrames));
        if (fadeOutFrames > 0 && index + fadeOutFrames >= totalFrames) {
            const double remaining = index < totalFrames ? static_cast<double>(totalFrames - index) : 0.0;
            const double t = std::min(1.0, remaining / static_cast<double>(fadeOutFrames));
            gain *= std::sin(kPi / 2.0 * t);
        }
        if (gain != 1.0)
            for (uint32_t ch = 0; ch < channels; ++ch)
                interleaved[f * channels + ch] = static_cast<float>(interleaved[f * channels + ch] * gain);
    }
}

}
