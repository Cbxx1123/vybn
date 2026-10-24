#include "audio/Stretch.h"

#include <algorithm>
#include <cmath>

#include "audio/ClipPlayer.h"

namespace audio {

namespace {

constexpr double kPi = 3.141592653589793;
constexpr double kTwoPi = 6.283185307179586;
// The path is worked out every this many frames and joined by lines in between
constexpr uint32_t kPathStep = 32;

// Frame `index` of the file (silence outside it); false when its chunk isn't in memory
inline bool frameOf(const AudioData &d, int64_t index, AudioData::Span &span, float &l, float &r)
{
    if (index < 0 || static_cast<uint64_t>(index) >= d.frames) {
        l = r = 0.0f;
        return true;
    }
    const float *p = d.frameAt(static_cast<uint64_t>(index), span);
    if (!p) {
        l = r = 0.0f;
        return false;
    }
    l = p[0];
    r = d.channels > 1 ? p[1] : p[0];
    return true;
}

// Between two frames, as unwarped clips are read (nothing before the file, nor at its last frame)
inline void readLinear(const AudioData &d, double pos, AudioData::Span &span, float &l, float &r)
{
    l = r = 0.0f;
    if (pos < 0.0)
        return;
    const auto index = static_cast<uint64_t>(pos);
    if (index + 1 >= d.frames)
        return;
    const float frac = static_cast<float>(pos - static_cast<double>(index));
    float al, ar, bl, br;
    if (!frameOf(d, static_cast<int64_t>(index), span, al, ar) || !frameOf(d, static_cast<int64_t>(index) + 1, span, bl, br))
        return;
    l = al + (bl - al) * frac;
    r = ar + (br - ar) * frac;
}

// Four frames through a Catmull-Rom curve, for reading at any speed
inline void readHermite(const AudioData &d, double pos, AudioData::Span &span, float &l, float &r)
{
    l = r = 0.0f;
    if (pos <= -2.0 || pos >= static_cast<double>(d.frames) + 1.0)
        return;
    const double fl = std::floor(pos);
    const auto i = static_cast<int64_t>(fl);
    const float f = static_cast<float>(pos - fl);
    float l0, r0, l1, r1, l2, r2, l3, r3;
    frameOf(d, i - 1, span, l0, r0);
    frameOf(d, i, span, l1, r1);
    frameOf(d, i + 1, span, l2, r2);
    frameOf(d, i + 2, span, l3, r3);
    auto curve = [f](float xm1, float x0, float x1, float x2) {
        const float c1 = 0.5f * (x1 - xm1);
        const float c2 = xm1 - 2.5f * x0 + 2.0f * x1 - 0.5f * x2;
        const float c3 = 0.5f * (x2 - xm1) + 1.5f * (x0 - x1);
        return ((c3 * f + c2) * f + c1) * f + x0;
    };
    l = curve(l0, l1, l2, l3);
    r = curve(r0, r1, r2, r3);
}

inline float readMono(const AudioData &d, double pos, AudioData::Span &span)
{
    float l, r;
    readHermite(d, pos, span, l, r);
    return 0.5f * (l + r);
}

inline double princarg(double phase)
{
    return phase - kTwoPi * std::floor((phase + kPi) / kTwoPi);
}

// The path's file positions at every kPathStep frames of [from, to], joined by lines
class PathPositions {
public:
    PathPositions(const WarpPath &path, uint32_t from, uint32_t to) : from_(from), to_(to)
    {
        count_ = 0;
        for (uint32_t f = from;; f += kPathStep) {
            const uint32_t at = std::min(f, to);
            p_[count_++] = path.at(at);
            if (at >= to)
                break;
        }
    }
    double at(uint32_t i) const
    {
        const uint32_t k = (i - from_) / kPathStep;
        if (k + 1 >= count_)
            return p_[count_ - 1];
        const uint32_t a = from_ + k * kPathStep;
        const uint32_t b = std::min(a + kPathStep, to_);
        return p_[k] + (p_[k + 1] - p_[k]) * static_cast<double>(i - a) / static_cast<double>(b - a);
    }

private:
    uint32_t from_, to_, count_;
    double p_[kMaxStretchBlock / kPathStep + 2];
};

// Tones: WSOLA. Hann grains half overlapping, each from where the path says at its middle, nudged within a
// quarter grain to where it best continues the grain before, read at the transposed speed.
class TonesVoice final : public StretchVoice {
public:
    static constexpr size_t kMaxGrain = 8192;
    static constexpr size_t kRing = 16384; // more than a grain ahead of the frame being played

    TonesVoice()
        : StretchVoice(WarpMode::Tones), ringLeft_(kRing), ringRight_(kRing), window_(kMaxGrain),
          reference_(kMaxGrain / 8 + 1), search_(kMaxGrain + kMaxGrain / 2 + 8)
    {
    }

protected:
    void stretch(const ClipRegion &region, const WarpPath &path, const TransportState &t, uint32_t from, uint32_t to,
                 float *left, float *right, bool fresh) override
    {
        const size_t grain = grainFor(region.warp.grain, t.sampleRate);
        if (grain != grain_) {
            grain_ = grain;
            for (size_t j = 0; j < grain_; ++j)
                window_[j] = static_cast<float>(0.5 - 0.5 * std::cos(kTwoPi * static_cast<double>(j) / static_cast<double>(grain_)));
            fresh = true;
        }
        const auto hop = static_cast<int64_t>(grain_ / 2);
        const auto base = static_cast<int64_t>(t.position);
        if (fresh) {
            std::fill(ringLeft_.begin(), ringLeft_.end(), 0.0f);
            std::fill(ringRight_.begin(), ringRight_.end(), 0.0f);
            // The grains already sounding at `from`, so it starts full
            next_ = base + from - static_cast<int64_t>(grain_) + hop;
            havePrevious_ = false;
        }
        const double step = stretchStep(region, t.sampleRate);
        for (uint32_t i = from; i < to; ++i) {
            const int64_t now = base + i;
            while (next_ <= now) {
                grainAt(region, path, base, next_, now, step);
                next_ += hop;
            }
            const size_t slot = static_cast<size_t>(now) & (kRing - 1);
            left[i] = ringLeft_[slot];
            right[i] = ringRight_[slot];
            ringLeft_[slot] = ringRight_[slot] = 0.0f;
        }
    }

private:
    static size_t grainFor(float grain, double rate)
    {
        const double ms = 12.0 * std::pow(90.0 / 12.0, std::clamp(grain, 0.0f, 100.0f) / 100.0);
        const auto n = static_cast<size_t>(2 * std::lround(ms * rate / 2000.0));
        return std::clamp<size_t>(n, 64, kMaxGrain);
    }

    void grainAt(const ClipRegion &region, const WarpPath &path, int64_t base, int64_t g, int64_t now, double step)
    {
        const AudioData &d = *region.data;
        const size_t n = grain_;
        const size_t h = n / 2;
        const double half = static_cast<double>(h);
        const double nominal = path.at(static_cast<double>(g - base) + half) - half * step;
        double start = nominal;
        if (havePrevious_) {
            const double expect = previous_ + half * step;
            if (std::fabs(nominal - expect) < static_cast<double>(n) * step) {
                // Where the grain's first half best matches what the grain before would go on to play
                const auto reach = static_cast<int64_t>(n / 4);
                size_t m = 0;
                for (size_t j = 0; j < h; j += 4)
                    reference_[m++] = readMono(d, expect + static_cast<double>(j) * step, span_);
                const size_t count = static_cast<size_t>(2 * reach) + h;
                for (size_t k = 0; k < count; ++k)
                    search_[k] = readMono(d, nominal + (static_cast<double>(k) - static_cast<double>(reach)) * step, span_);
                int64_t best = 0;
                double bestScore = -1e30;
                for (int64_t lag = -reach; lag <= reach; lag += 2) {
                    double corr = 0.0, energy = 0.0;
                    const float *s = search_.data() + (lag + reach);
                    size_t q = 0;
                    for (size_t j = 0; j < h; j += 4, ++q) {
                        corr += static_cast<double>(reference_[q]) * s[j];
                        energy += static_cast<double>(s[j]) * s[j];
                    }
                    if (energy < 1e-12)
                        continue;
                    // A slight lean toward the nominal place, so silence and noise don't wander
                    const double score = corr / std::sqrt(energy) - 1e-4 * std::fabs(static_cast<double>(lag));
                    if (score > bestScore) {
                        bestScore = score;
                        best = lag;
                    }
                }
                start = nominal + static_cast<double>(best) * step;
            }
        }
        for (size_t j = 0; j < n; ++j) {
            const int64_t at = g + static_cast<int64_t>(j);
            if (at < now)
                continue; // already played (a cold start's earlier grains)
            float l, r;
            readHermite(d, start + static_cast<double>(j) * step, span_, l, r);
            const size_t slot = static_cast<size_t>(at) & (kRing - 1);
            ringLeft_[slot] += l * window_[j];
            ringRight_[slot] += r * window_[j];
        }
        previous_ = start;
        havePrevious_ = true;
    }

    std::vector<float> ringLeft_, ringRight_, window_, reference_, search_;
    size_t grain_ = 0;
    int64_t next_ = 0;
    double previous_ = 0.0;
    bool havePrevious_ = false;
    AudioData::Span span_;
};

// Complex: a phase vocoder. Each frame reads the file around where the path says, and one hop of
// grain time before it, so each peak's true frequency comes from the pair; the other bins keep their
// phase against their peak (identity phase locking); the phases come from the channels' middle and
// rotate both channels alike, so the stereo image stays; transients reset them.
class ComplexVoice final : public StretchVoice {
public:
    static constexpr size_t kMaxFrame = 4096;
    static constexpr size_t kRing = 8192;
    static constexpr size_t kBins = kMaxFrame / 2 + 1;

    ComplexVoice()
        : StretchVoice(WarpMode::Complex), small_(2048), large_(4096), window_(kMaxFrame), left_(kMaxFrame),
          right_(kMaxFrame), back_(kMaxFrame), out_(kMaxFrame), reL_(kBins), imL_(kBins), reR_(kBins), imR_(kBins),
          reB_(kBins), imB_(kBins), phaseA_(kBins), phaseS_(kBins), peakPhase_(kBins), magnitude_(kBins), peaks_(kBins),
          ringLeft_(kRing), ringRight_(kRing)
    {
    }

protected:
    void stretch(const ClipRegion &region, const WarpPath &path, const TransportState &t, uint32_t from, uint32_t to,
                 float *left, float *right, bool fresh) override
    {
        const size_t size = t.sampleRate > 64000.0 ? 4096 : 2048;
        if (size != size_) {
            size_ = size;
            for (size_t j = 0; j < size_; ++j)
                window_[j] = static_cast<float>(0.5 - 0.5 * std::cos(kTwoPi * static_cast<double>(j) / static_cast<double>(size_)));
            fresh = true;
        }
        const auto hop = static_cast<int64_t>(size_ / 4);
        const auto base = static_cast<int64_t>(t.position);
        if (fresh) {
            std::fill(ringLeft_.begin(), ringLeft_.end(), 0.0f);
            std::fill(ringRight_.begin(), ringRight_.end(), 0.0f);
            next_ = base + from - static_cast<int64_t>(size_) + hop;
            havePhase_ = false;
        }
        const double step = stretchStep(region, t.sampleRate);
        for (uint32_t i = from; i < to; ++i) {
            const int64_t now = base + i;
            while (next_ <= now) {
                frameAt(region, path, base, next_, now, step);
                next_ += hop;
            }
            const size_t slot = static_cast<size_t>(now) & (kRing - 1);
            left[i] = ringLeft_[slot];
            right[i] = ringRight_[slot];
            ringLeft_[slot] = ringRight_[slot] = 0.0f;
        }
    }

private:
    void frameAt(const ClipRegion &region, const WarpPath &path, int64_t base, int64_t g, int64_t now, double step)
    {
        const AudioData &d = *region.data;
        const bool stereo = d.channels > 1;
        const size_t n = size_;
        const size_t hop = n / 4;
        const size_t bins = n / 2 + 1;
        Fft &fft = n == 4096 ? large_ : small_;
        const double half = static_cast<double>(n / 2);
        const double centre = path.at(static_cast<double>(g - base) + half);
        const double back = static_cast<double>(hop) * step;
        for (size_t j = 0; j < n; ++j) {
            const double pos = centre + (static_cast<double>(j) - half) * step;
            float l, r, bl, br;
            readHermite(d, pos, span_, l, r);
            readHermite(d, pos - back, span_, bl, br);
            left_[j] = l * window_[j];
            right_[j] = r * window_[j];
            back_[j] = 0.5f * (bl + br) * window_[j];
        }
        fft.forward(left_.data(), reL_.data(), imL_.data());
        if (stereo)
            fft.forward(right_.data(), reR_.data(), imR_.data());
        fft.forward(back_.data(), reB_.data(), imB_.data());

        float peakMagnitude = 0.0f;
        for (size_t k = 0; k < bins; ++k) {
            const float mr = stereo ? 0.5f * (reL_[k] + reR_[k]) : reL_[k];
            const float mi = stereo ? 0.5f * (imL_[k] + imR_[k]) : imL_[k];
            phaseA_[k] = std::atan2(mi, mr);
            magnitude_[k] = std::sqrt(mr * mr + mi * mi);
            peakMagnitude = std::max(peakMagnitude, magnitude_[k]);
        }

        bool reset = !havePhase_;
        if (!reset && region.resets && !region.resets->empty()) {
            // A transient between the last frame's middle and this one's: the phases start again there
            const double lo = std::min(previous_, centre), hi = std::max(previous_, centre);
            const auto &r = *region.resets;
            const auto it = std::upper_bound(r.begin(), r.end(), lo);
            reset = it != r.end() && *it <= hi;
        }
        if (!reset && std::fabs(centre - previous_) > static_cast<double>(n) * step * 4.0)
            reset = true; // a jump in the file
        if (reset) {
            for (size_t k = 0; k < bins; ++k)
                phaseS_[k] = phaseA_[k];
        } else {
            // The peaks, and each one's phase moved on by its true frequency over a hop
            size_t count = 0;
            const float floor = peakMagnitude * 1e-5f;
            for (size_t k = 2; k + 2 < bins; ++k) {
                const float m = magnitude_[k];
                if (m > floor && m > magnitude_[k - 1] && m >= magnitude_[k + 1] && m > magnitude_[k - 2] && m >= magnitude_[k + 2])
                    peaks_[count++] = k;
            }
            if (count == 0) {
                for (size_t k = 0; k < bins; ++k)
                    phaseS_[k] = phaseA_[k];
            } else {
                const double hopD = static_cast<double>(hop);
                for (size_t p = 0; p < count; ++p) {
                    const size_t k = peaks_[p];
                    const double omega = kTwoPi * static_cast<double>(k) / static_cast<double>(n);
                    const double phaseB = std::atan2(imB_[k], reB_[k]);
                    const double deviation = princarg(phaseA_[k] - phaseB - omega * hopD);
                    peakPhase_[k] = static_cast<float>(princarg(phaseS_[k] + omega * hopD + deviation));
                }
                // Every bin keeps its phase against the peak whose region it is in
                size_t p = 0;
                for (size_t k = 0; k < bins; ++k) {
                    while (p + 1 < count && k * 2 >= peaks_[p] + peaks_[p + 1])
                        ++p;
                    const size_t peak = peaks_[p];
                    phaseS_[k] = static_cast<float>(princarg(peakPhase_[peak] + phaseA_[k] - phaseA_[peak]));
                }
            }
        }

        // Both channels turned by the same angle per bin, back to time, windowed and overlapped
        // (Hann squared at four times overlap sums to 1.5)
        for (size_t k = 0; k < bins; ++k) {
            const float angle = phaseS_[k] - phaseA_[k];
            const float c = std::cos(angle), s = std::sin(angle);
            const float lr = reL_[k], li = imL_[k];
            reL_[k] = lr * c - li * s;
            imL_[k] = lr * s + li * c;
            if (stereo) {
                const float rr = reR_[k], ri = imR_[k];
                reR_[k] = rr * c - ri * s;
                imR_[k] = rr * s + ri * c;
            }
        }
        constexpr float kScale = 1.0f / 1.5f;
        fft.inverse(reL_.data(), imL_.data(), out_.data());
        for (size_t j = 0; j < n; ++j) {
            const int64_t at = g + static_cast<int64_t>(j);
            if (at < now)
                continue;
            const size_t slot = static_cast<size_t>(at) & (kRing - 1);
            const float v = out_[j] * window_[j] * kScale;
            ringLeft_[slot] += v;
            if (!stereo)
                ringRight_[slot] += v;
        }
        if (stereo) {
            fft.inverse(reR_.data(), imR_.data(), out_.data());
            for (size_t j = 0; j < n; ++j) {
                const int64_t at = g + static_cast<int64_t>(j);
                if (at < now)
                    continue;
                ringRight_[static_cast<size_t>(at) & (kRing - 1)] += out_[j] * window_[j] * kScale;
            }
        }
        previous_ = centre;
        havePhase_ = true;
    }

    Fft small_, large_;
    std::vector<float> window_, left_, right_, back_, out_;
    std::vector<float> reL_, imL_, reR_, imR_, reB_, imB_, phaseA_, phaseS_, peakPhase_, magnitude_;
    std::vector<size_t> peaks_;
    std::vector<float> ringLeft_, ringRight_;
    size_t size_ = 0;
    int64_t next_ = 0;
    double previous_ = 0.0;
    bool havePhase_ = false;
    AudioData::Span span_;
};

// Beats: each slice starts on the beat its first frame is pinned to and plays at the file's speed
// (transposed by reading faster or slower); one longer than its sound ends, loops forward or back and
// forth; one cut short fades out under the next.
class BeatsVoice final : public StretchVoice {
public:
    BeatsVoice() : StretchVoice(WarpMode::Beats) {}

protected:
    void stretch(const ClipRegion &region, const WarpPath &path, const TransportState &t, uint32_t from, uint32_t to,
                 float *left, float *right, bool fresh) override
    {
        if (!region.slices || region.slices->empty()) {
            std::fill(left + from, left + to, 0.0f);
            std::fill(right + from, right + to, 0.0f);
            return;
        }
        const std::vector<WarpSlice> &s = *region.slices;
        const AudioData &d = *region.data;
        const double step = stretchStep(region, t.sampleRate);
        const auto crossfade = static_cast<uint32_t>(std::max(1.0, 0.003 * t.sampleRate));
        const double tau = region.warp.envelope >= 100.0f
                               ? 0.0
                               : 0.01 * std::pow(200.0, region.warp.envelope / 100.0) * t.sampleRate;
        if (fresh) {
            slice_ = -2;
            fade_ = 0;
        }
        const double c0 = path.content(from);
        const double c1 = path.content(to);
        const double span = static_cast<double>(to - from);
        for (uint32_t i = from; i < to; ++i) {
            const double c = c0 + (c1 - c0) * static_cast<double>(i - from) / span;
            const int k = sliceAt(s, c);
            if (k != slice_) {
                if (slice_ >= 0) {
                    previous_ = slice_;
                    previousElapsed_ = elapsed_;
                    fade_ = crossfade;
                }
                slice_ = k;
                if (k >= 0) {
                    const double begins = t.frameAt(region.start + s[static_cast<size_t>(k)].beat - region.contentStart);
                    elapsed_ = std::max(0.0, static_cast<double>(i) - begins);
                    // The file's frames its time holds, against which its sound may run out
                    slot_ = static_cast<size_t>(k) + 1 < s.size()
                                ? (t.frameAt(region.start + s[static_cast<size_t>(k) + 1].beat - region.contentStart) - begins) * step
                                : 1e18;
                }
            } else {
                elapsed_ += 1.0;
            }
            float l = 0.0f, r = 0.0f;
            play(region, d, s, slice_, elapsed_, step, tau, slot_, false, l, r);
            if (fade_ > 0) {
                // The new slice starts at full level, so its hit stays sharp; the one before fades out
                const double x = 1.0 - static_cast<double>(fade_) / crossfade;
                const auto out = static_cast<float>(std::cos(x * kPi * 0.5));
                float pl = 0.0f, pr = 0.0f;
                play(region, d, s, previous_, previousElapsed_, step, tau, 0.0, true, pl, pr);
                l += pl * out;
                r += pr * out;
                previousElapsed_ += 1.0;
                --fade_;
            }
            left[i] = l;
            right[i] = r;
        }
    }

private:
    int sliceAt(const std::vector<WarpSlice> &s, double c) const
    {
        const int n = static_cast<int>(s.size());
        auto inside = [&](int k) {
            return k >= 0 && c >= s[static_cast<size_t>(k)].beat && (k + 1 == n || c < s[static_cast<size_t>(k) + 1].beat);
        };
        if (inside(slice_))
            return slice_;
        if (slice_ >= -1 && inside(slice_ + 1))
            return slice_ + 1;
        const auto next = std::upper_bound(s.begin(), s.end(), c, [](double b, const WarpSlice &w) { return b < w.beat; });
        return static_cast<int>(next - s.begin()) - 1;
    }

    void play(const ClipRegion &region, const AudioData &d, const std::vector<WarpSlice> &s, int k, double elapsed,
              double step, double tau, double slot, bool tail, float &l, float &r)
    {
        l = r = 0.0f;
        if (k < 0)
            return;
        const double begin = s[static_cast<size_t>(k)].frame;
        const double end = static_cast<size_t>(k) + 1 < s.size() ? s[static_cast<size_t>(k) + 1].frame : static_cast<double>(d.frames);
        const double length = std::max(1.0, end - begin);
        const double played = elapsed * step;
        float gain = tau > 0.0 ? static_cast<float>(std::exp(-elapsed / tau)) : 1.0f;
        double pos = begin + played;
        double other = -1.0; // a second place read for a loop's crossfade, and its share
        float share = 0.0f;
        WarpLoop mode = region.warp.loop;
        const double half = length * 0.5;
        const double loop = length - half;
        if (loop < 16.0)
            mode = WarpLoop::Off;
        // A slice whose time outlasts its sound fades out just before the sound runs into the next
        // slice's (one cut short is faded out under the next instead)
        if (!tail && mode == WarpLoop::Off && slot > length + 1.0) {
            const double g = std::min(1.0, (length - played) / (0.004 * d.sampleRate));
            if (g <= 0.0)
                return;
            gain *= static_cast<float>(g);
        }
        if (!tail && played >= length && slot > length + 1.0) {
            switch (mode) {
            case WarpLoop::Off:
                break;
            case WarpLoop::Forward: {
                const double phase = std::fmod(played - half, loop);
                pos = begin + half + phase;
                const double xf = std::min(loop * 0.5, 0.002 * d.sampleRate);
                if (phase > loop - xf) {
                    other = pos - loop;
                    share = static_cast<float>((phase - (loop - xf)) / xf);
                }
                break;
            }
            case WarpLoop::BackAndForth: {
                const double m = std::fmod(played - half, 2.0 * loop);
                pos = begin + half + (m < loop ? m : 2.0 * loop - m);
                break;
            }
            }
        }
        readHermite(d, pos, span_, l, r);
        if (other >= 0.0) {
            float ol, orr;
            readHermite(d, other, span_, ol, orr);
            l = l * (1.0f - share) + ol * share;
            r = r * (1.0f - share) + orr * share;
        }
        l *= gain;
        r *= gain;
    }

    int slice_ = -2; // -2: not yet found; -1: before the first
    double elapsed_ = 0.0;
    int previous_ = -1;
    double previousElapsed_ = 0.0;
    double slot_ = 1e18;
    uint32_t fade_ = 0;
    AudioData::Span span_;
};

}

double WarpPath::content(double frame) const
{
    return region->contentStart + transport->beatAt(frame) - region->start;
}

double WarpPath::at(double frame) const
{
    return warpSeconds(region->warp, content(frame)) * region->data->sampleRate;
}

double stretchStep(const ClipRegion &region, double deviceRate)
{
    return std::pow(2.0, region.warp.pitch / 12.0) * region.data->sampleRate / deviceRate;
}

StretchVoice::StretchVoice(WarpMode mode) : mode_(mode), otherLeft_(kMaxStretchBlock), otherRight_(kMaxStretchBlock) {}

void StretchVoice::unbind()
{
    key = 0;
    used = false;
    running_ = false;
    started_ = false;
    fade_ = 0;
}

void StretchVoice::render(const ClipRegion &region, const WarpPath &path, const TransportState &t, uint32_t from,
                          uint32_t to, float *left, float *right)
{
    if (to <= from)
        return;
    // Played at the file's own speed and pitch: read directly
    const double unit = region.data->sampleRate / t.sampleRate;
    const double p0 = path.at(from);
    const double p1 = path.at(to);
    const double pm = path.at(0.5 * (from + to));
    const double frames = static_cast<double>(to - from);
    const bool identity = region.warp.pitch == 0.0 && std::fabs((p1 - p0) / frames / unit - 1.0) < 1e-6 &&
                          std::fabs((pm - p0) / (0.5 * frames) / unit - 1.0) < 1e-6;
    const bool target = !identity;
    fadeLength_ = static_cast<uint32_t>(std::max(1.0, kSwitchSeconds * t.sampleRate));
    if (!started_) {
        stretching_ = target;
        fade_ = 0;
        started_ = true;
    } else if (target != stretching_) {
        stretching_ = target;
        fade_ = fadeLength_;
    }
    const bool runEngine = stretching_ || fade_ > 0;
    const bool runDirect = !stretching_ || fade_ > 0;
    // The target into left and right, what it fades from into the other buffers
    float *engineLeft = stretching_ ? left : otherLeft_.data();
    float *engineRight = stretching_ ? right : otherRight_.data();
    float *directLeft = stretching_ ? otherLeft_.data() : left;
    float *directRight = stretching_ ? otherRight_.data() : right;
    if (runEngine) {
        const bool fresh = !running_ || next_ != t.position + from;
        stretch(region, path, t, from, to, engineLeft, engineRight, fresh);
        running_ = true;
        next_ = t.position + to;
    } else {
        running_ = false;
    }
    if (runDirect)
        renderDirect(region, path, from, to, directLeft, directRight);
    for (uint32_t i = from; i < to && fade_ > 0; ++i, --fade_) {
        const double x = 1.0 - static_cast<double>(fade_) / fadeLength_;
        const auto in = static_cast<float>(std::sin(x * kPi * 0.5));
        const auto out = static_cast<float>(std::cos(x * kPi * 0.5));
        left[i] = left[i] * in + otherLeft_[i] * out;
        right[i] = right[i] * in + otherRight_[i] * out;
    }
}

std::unique_ptr<StretchVoice> makeStretchVoice(WarpMode mode)
{
    switch (mode) {
    case WarpMode::Beats:
        return std::make_unique<BeatsVoice>();
    case WarpMode::Tones:
        return std::make_unique<TonesVoice>();
    case WarpMode::Complex:
        return std::make_unique<ComplexVoice>();
    case WarpMode::Repitch:
        break;
    }
    return nullptr;
}

void renderDirect(const ClipRegion &region, const WarpPath &path, uint32_t from, uint32_t to, float *left, float *right)
{
    const PathPositions positions(path, from, to);
    AudioData::Span span;
    for (uint32_t i = from; i < to; ++i)
        readLinear(*region.data, positions.at(i), span, left[i], right[i]);
}

void renderRepitch(const ClipRegion &region, const WarpPath &path, uint32_t from, uint32_t to, float *left, float *right)
{
    const PathPositions positions(path, from, to);
    AudioData::Span span;
    for (uint32_t i = from; i < to; ++i)
        readHermite(*region.data, positions.at(i), span, left[i], right[i]);
}

}
