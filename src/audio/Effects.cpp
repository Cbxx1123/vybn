#include "Effects.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace audio {

namespace {

constexpr double kPi = 3.14159265358979323846;

using P = ParamInfo;
constexpr ParamUnit dB = ParamUnit::Decibels;
constexpr ParamUnit Hz = ParamUnit::Hertz;
constexpr ParamUnit ms = ParamUnit::Milliseconds;
constexpr ParamUnit pct = ParamUnit::Percent;

const EffectInfo kInfo[kEffectTypeCount] = {
    {"Gain", "gain", 2,
     {P{"Gain", "gain", -60.0f, 24.0f, 0.0f, dB}, P{"Invert", "invert", 0.0f, 1.0f, 0.0f, ParamUnit::Toggle}}, false},
    {"EQ", "eq", 7,
     {P{"Low freq", "low_freq", 20.0f, 1000.0f, 200.0f, Hz, true}, P{"Low gain", "low_gain", -18.0f, 18.0f, 0.0f, dB},
      P{"Mid freq", "mid_freq", 100.0f, 8000.0f, 1000.0f, Hz, true}, P{"Mid gain", "mid_gain", -18.0f, 18.0f, 0.0f, dB},
      P{"Mid Q", "mid_q", 0.2f, 10.0f, 1.0f, ParamUnit::None, true},
      P{"High freq", "high_freq", 1000.0f, 18000.0f, 5000.0f, Hz, true},
      P{"High gain", "high_gain", -18.0f, 18.0f, 0.0f, dB}},
     false},
    {"Filter", "filter", 3,
     {P{"Type", "type", 0.0f, 2.0f, 0.0f, ParamUnit::Choice, false, "Low-pass|High-pass|Band-pass"},
      P{"Cutoff", "cutoff", 20.0f, 20000.0f, 8000.0f, Hz, true},
      P{"Resonance", "resonance", 0.5f, 10.0f, 0.707f, ParamUnit::None, true}},
     false},
    {"Gate", "gate", 4,
     {P{"Threshold", "threshold", -80.0f, 0.0f, -50.0f, dB}, P{"Attack", "attack", 0.1f, 50.0f, 1.0f, ms, true},
      P{"Release", "release", 5.0f, 1000.0f, 100.0f, ms, true}, P{"Range", "range", -80.0f, 0.0f, -80.0f, dB}},
     true},
    {"Compressor", "compressor", 7,
     {P{"Threshold", "threshold", -60.0f, 0.0f, -18.0f, dB}, P{"Ratio", "ratio", 1.0f, 20.0f, 4.0f, ParamUnit::Ratio, true},
      P{"Attack", "attack", 0.1f, 200.0f, 10.0f, ms, true}, P{"Release", "release", 10.0f, 2000.0f, 120.0f, ms, true},
      P{"Knee", "knee", 0.0f, 24.0f, 6.0f, dB}, P{"Makeup", "makeup", 0.0f, 24.0f, 0.0f, dB},
      P{"Mix", "mix", 0.0f, 100.0f, 100.0f, pct}},
     true},
    {"Limiter", "limiter", 3,
     {P{"Input", "input", 0.0f, 24.0f, 0.0f, dB}, P{"Ceiling", "ceiling", -12.0f, 0.0f, -0.3f, dB},
      P{"Release", "release", 10.0f, 1000.0f, 80.0f, ms, true}},
     true},
    {"Saturation", "saturation", 4,
     {P{"Type", "type", 0.0f, 2.0f, 0.0f, ParamUnit::Choice, false, "Soft|Hard|Fold"}, P{"Drive", "drive", 0.0f, 36.0f, 6.0f, dB},
      P{"Mix", "mix", 0.0f, 100.0f, 100.0f, pct}, P{"Output", "output", -24.0f, 12.0f, 0.0f, dB}},
     false},
    {"Delay", "delay", 5,
     {P{"Time", "time", 1.0f, 2000.0f, 350.0f, ms, true}, P{"Feedback", "feedback", 0.0f, 95.0f, 35.0f, pct},
      P{"Tone", "tone", 500.0f, 18000.0f, 6000.0f, Hz, true}, P{"Ping-pong", "pingpong", 0.0f, 1.0f, 0.0f, ParamUnit::Toggle},
      P{"Mix", "mix", 0.0f, 100.0f, 25.0f, pct}},
     false},
    {"Reverb", "reverb", 4,
     {P{"Size", "size", 0.0f, 100.0f, 50.0f, pct}, P{"Damping", "damping", 0.0f, 100.0f, 50.0f, pct},
      P{"Width", "width", 0.0f, 100.0f, 100.0f, pct}, P{"Mix", "mix", 0.0f, 100.0f, 25.0f, pct}},
     false},
};

// Biquad filters (RBJ's cookbook), one state per channel
struct Biquad {
    double b0 = 1.0, b1 = 0.0, b2 = 0.0, a1 = 0.0, a2 = 0.0;
    double z1[2] = {}, z2[2] = {};

    void reset() { z1[0] = z1[1] = z2[0] = z2[1] = 0.0; }
    void set(double nb0, double nb1, double nb2, double na0, double na1, double na2)
    {
        b0 = nb0 / na0;
        b1 = nb1 / na0;
        b2 = nb2 / na0;
        a1 = na1 / na0;
        a2 = na2 / na0;
    }
    static double clampFrequency(double sampleRate, double f) { return std::clamp(f, 10.0, sampleRate * 0.49); }

    void peaking(double sr, double f, double gainDb, double q)
    {
        const double w = 2.0 * kPi * clampFrequency(sr, f) / sr, c = std::cos(w), alpha = std::sin(w) / (2.0 * q);
        const double A = std::pow(10.0, gainDb / 40.0);
        set(1.0 + alpha * A, -2.0 * c, 1.0 - alpha * A, 1.0 + alpha / A, -2.0 * c, 1.0 - alpha / A);
    }
    void lowShelf(double sr, double f, double gainDb)
    {
        const double w = 2.0 * kPi * clampFrequency(sr, f) / sr, c = std::cos(w), s = std::sin(w);
        const double A = std::pow(10.0, gainDb / 40.0), beta = 2.0 * std::sqrt(A) * (s / 2.0 * std::sqrt(2.0));
        set(A * ((A + 1) - (A - 1) * c + beta), 2 * A * ((A - 1) - (A + 1) * c), A * ((A + 1) - (A - 1) * c - beta),
            (A + 1) + (A - 1) * c + beta, -2 * ((A - 1) + (A + 1) * c), (A + 1) + (A - 1) * c - beta);
    }
    void highShelf(double sr, double f, double gainDb)
    {
        const double w = 2.0 * kPi * clampFrequency(sr, f) / sr, c = std::cos(w), s = std::sin(w);
        const double A = std::pow(10.0, gainDb / 40.0), beta = 2.0 * std::sqrt(A) * (s / 2.0 * std::sqrt(2.0));
        set(A * ((A + 1) + (A - 1) * c + beta), -2 * A * ((A - 1) + (A + 1) * c), A * ((A + 1) + (A - 1) * c - beta),
            (A + 1) - (A - 1) * c + beta, 2 * ((A - 1) - (A + 1) * c), (A + 1) - (A - 1) * c - beta);
    }
    void lowpass(double sr, double f, double q)
    {
        const double w = 2.0 * kPi * clampFrequency(sr, f) / sr, c = std::cos(w), alpha = std::sin(w) / (2.0 * q);
        set((1 - c) / 2, 1 - c, (1 - c) / 2, 1 + alpha, -2 * c, 1 - alpha);
    }
    void highpass(double sr, double f, double q)
    {
        const double w = 2.0 * kPi * clampFrequency(sr, f) / sr, c = std::cos(w), alpha = std::sin(w) / (2.0 * q);
        set((1 + c) / 2, -(1 + c), (1 + c) / 2, 1 + alpha, -2 * c, 1 - alpha);
    }
    void bandpass(double sr, double f, double q)
    {
        const double w = 2.0 * kPi * clampFrequency(sr, f) / sr, c = std::cos(w), alpha = std::sin(w) / (2.0 * q);
        set(alpha, 0.0, -alpha, 1 + alpha, -2 * c, 1 - alpha);
    }

    float tick(int channel, float x)
    {
        const double y = b0 * x + z1[channel];
        z1[channel] = b1 * x - a1 * y + z2[channel];
        z2[channel] = b2 * x - a2 * y;
        return static_cast<float>(y);
    }
};

// A time constant in milliseconds as a per-sample smoothing coefficient (0 jumps, near 1 is slow)
float timeCoefficient(double sampleRate, double milliseconds)
{
    return static_cast<float>(std::exp(-1.0 / (std::max(0.01, milliseconds) * 0.001 * sampleRate)));
}

float dbToLinear(float db)
{
    return std::pow(10.0f, db / 20.0f);
}

// Gain: a level and a phase flip
class GainEffect : public BuiltinEffect {
public:
    GainEffect() : BuiltinEffect(EffectType::Gain) {}

protected:
    void onPrepare(double sampleRate, uint32_t) override
    {
        level_.prepare(sampleRate, 10.0);
        reset();
    }
    void reset() override { level_.reset(target()); }
    void process(float *left, float *right, uint32_t frames) override
    {
        const float t = target();
        for (uint32_t i = 0; i < frames; ++i) {
            const float g = level_.next(t);
            left[i] *= g;
            right[i] *= g;
        }
    }

private:
    float target() const { return dbToLinear(value(0)) * (value(1) >= 0.5f ? -1.0f : 1.0f); }
    Smoothed level_;
};

// EQ: a low shelf, a peak and a high shelf
class EqEffect : public BuiltinEffect {
public:
    EqEffect() : BuiltinEffect(EffectType::Equalizer) {}

protected:
    void onPrepare(double, uint32_t) override { cached_.fill(-1e9f); }
    void reset() override
    {
        low_.reset();
        mid_.reset();
        high_.reset();
    }
    void process(float *left, float *right, uint32_t frames) override
    {
        bool changed = false;
        for (size_t i = 0; i < 7; ++i) {
            if (value(i) != cached_[i]) {
                cached_[i] = value(i);
                changed = true;
            }
        }
        if (changed) {
            low_.lowShelf(sampleRate_, value(0), value(1));
            mid_.peaking(sampleRate_, value(2), value(3), value(4));
            high_.highShelf(sampleRate_, value(5), value(6));
        }
        for (uint32_t i = 0; i < frames; ++i) {
            left[i] = high_.tick(0, mid_.tick(0, low_.tick(0, left[i])));
            right[i] = high_.tick(1, mid_.tick(1, low_.tick(1, right[i])));
        }
    }

private:
    Biquad low_, mid_, high_;
    std::array<float, 7> cached_{};
};

// Filter: low-pass, high-pass or band-pass
class FilterEffect : public BuiltinEffect {
public:
    FilterEffect() : BuiltinEffect(EffectType::Filter) {}

protected:
    void onPrepare(double, uint32_t) override { cached_.fill(-1e9f); }
    void reset() override { filter_.reset(); }
    void process(float *left, float *right, uint32_t frames) override
    {
        bool changed = false;
        for (size_t i = 0; i < 3; ++i) {
            if (value(i) != cached_[i]) {
                cached_[i] = value(i);
                changed = true;
            }
        }
        if (changed) {
            const int type = static_cast<int>(value(0) + 0.5f);
            if (type == 1)
                filter_.highpass(sampleRate_, value(1), value(2));
            else if (type == 2)
                filter_.bandpass(sampleRate_, value(1), value(2));
            else
                filter_.lowpass(sampleRate_, value(1), value(2));
        }
        for (uint32_t i = 0; i < frames; ++i) {
            left[i] = filter_.tick(0, left[i]);
            right[i] = filter_.tick(1, right[i]);
        }
    }

private:
    Biquad filter_;
    std::array<float, 3> cached_{};
};

// Gate: closes (to the range) while the level is under the threshold
class GateEffect : public BuiltinEffect {
public:
    GateEffect() : BuiltinEffect(EffectType::Gate) {}

protected:
    void onPrepare(double sampleRate, uint32_t) override
    {
        detectorDecay_ = timeCoefficient(sampleRate, 20.0);
        holdSamples_ = static_cast<uint32_t>(sampleRate * 0.01);
        reset();
    }
    void reset() override
    {
        envelope_ = 0.0f;
        gain_ = 1.0f;
        open_ = true;
        hold_ = 0;
    }
    void process(float *left, float *right, uint32_t frames) override
    {
        const float threshold = value(0), closeThreshold = value(0) - 4.0f; // hysteresis
        const float attack = timeCoefficient(sampleRate_, value(1)), release = timeCoefficient(sampleRate_, value(2));
        const float floor = dbToLinear(value(3));
        float lowest = 1.0f;
        for (uint32_t i = 0; i < frames; ++i) {
            const float level = std::max(std::fabs(left[i]), std::fabs(right[i]));
            envelope_ = level > envelope_ ? level : level + (envelope_ - level) * detectorDecay_;
            const float db = toDecibels(envelope_);
            if (db > threshold) {
                open_ = true;
                hold_ = holdSamples_;
            } else if (db < closeThreshold && open_) {
                if (hold_ > 0)
                    --hold_;
                else
                    open_ = false;
            }
            const float target = open_ ? 1.0f : floor;
            const float coefficient = target > gain_ ? attack : release;
            gain_ = target + (gain_ - target) * coefficient;
            lowest = std::min(lowest, gain_);
            left[i] *= gain_;
            right[i] *= gain_;
        }
        setMeter(-toDecibels(lowest));
    }

private:
    float detectorDecay_ = 0.999f;
    uint32_t holdSamples_ = 480;
    float envelope_ = 0.0f, gain_ = 1.0f;
    bool open_ = true;
    uint32_t hold_ = 0;
};

// Compressor: a feed-forward peak compressor with a soft knee, smoothed in decibels
class CompressorEffect : public BuiltinEffect {
public:
    CompressorEffect() : BuiltinEffect(EffectType::Compressor) {}

protected:
    void onPrepare(double, uint32_t) override { reset(); }
    void reset() override { reduction_ = 0.0f; }
    void process(float *left, float *right, uint32_t frames) override
    {
        const float threshold = value(0), ratio = std::max(1.0f, value(1)), knee = value(4), makeup = value(5);
        const float attack = timeCoefficient(sampleRate_, value(2)), release = timeCoefficient(sampleRate_, value(3));
        const float mix = value(6) * 0.01f;
        const float slope = 1.0f / ratio - 1.0f; // negative
        float deepest = 0.0f;
        for (uint32_t i = 0; i < frames; ++i) {
            const float db = toDecibels(std::max(std::fabs(left[i]), std::fabs(right[i])));
            const float over = db - threshold;
            float target = 0.0f; // wanted gain change, in dB (zero or less)
            if (knee > 0.0f && 2.0f * over > -knee && 2.0f * over < knee) {
                const float x = over + knee * 0.5f;
                target = slope * x * x / (2.0f * knee);
            } else if (over > 0.0f && (knee <= 0.0f || 2.0f * over >= knee)) {
                target = slope * over;
            }
            const float coefficient = target < reduction_ ? attack : release;
            reduction_ = target + (reduction_ - target) * coefficient;
            deepest = std::min(deepest, reduction_);
            const float g = dbToLinear(reduction_ + makeup);
            left[i] = left[i] * (1.0f - mix) + left[i] * g * mix;
            right[i] = right[i] * (1.0f - mix) + right[i] * g * mix;
        }
        setMeter(-deepest);
    }

private:
    float reduction_ = 0.0f;
};

// Limiter: a lookahead limiter that holds the signal under the ceiling. The sound is delayed by
// about 1.5 ms while the gain comes down ahead of each peak; a last clamp catches anything the
// smoothing let through.
class LimiterEffect : public BuiltinEffect {
public:
    LimiterEffect() : BuiltinEffect(EffectType::Limiter) {}

protected:
    void onPrepare(double sampleRate, uint32_t) override
    {
        lookahead_ = std::max<size_t>(1, static_cast<size_t>(sampleRate * 0.0015));
        attack_ = static_cast<float>(std::exp(-1.0 / (static_cast<double>(lookahead_) / 6.0)));
        delay_[0].assign(lookahead_, 0.0f);
        delay_[1].assign(lookahead_, 0.0f);
        reset();
    }
    void reset() override
    {
        std::fill(delay_[0].begin(), delay_[0].end(), 0.0f);
        std::fill(delay_[1].begin(), delay_[1].end(), 0.0f);
        position_ = 0;
        gain_ = 1.0f;
    }
    void process(float *left, float *right, uint32_t frames) override
    {
        const float input = dbToLinear(value(0)), ceiling = dbToLinear(value(1));
        const float release = timeCoefficient(sampleRate_, value(2));
        float lowest = 1.0f;
        for (uint32_t i = 0; i < frames; ++i) {
            const float l = left[i] * input, r = right[i] * input;
            const float peak = std::max(std::fabs(l), std::fabs(r));
            const float needed = peak > ceiling ? ceiling / peak : 1.0f;
            if (needed < gain_)
                gain_ = needed + (gain_ - needed) * attack_;
            else
                gain_ = std::min(needed, 1.0f + (gain_ - 1.0f) * release);
            lowest = std::min(lowest, gain_);

            const float outL = delay_[0][position_], outR = delay_[1][position_];
            delay_[0][position_] = l;
            delay_[1][position_] = r;
            if (++position_ >= lookahead_)
                position_ = 0;
            left[i] = std::clamp(outL * gain_, -ceiling, ceiling);
            right[i] = std::clamp(outR * gain_, -ceiling, ceiling);
        }
        setMeter(-toDecibels(lowest));
    }

private:
    size_t lookahead_ = 72, position_ = 0;
    float attack_ = 0.9f, gain_ = 1.0f;
    std::vector<float> delay_[2];
};

// Saturation: soft clipping (tanh), hard clipping or wavefolding after a drive gain
class SaturationEffect : public BuiltinEffect {
public:
    SaturationEffect() : BuiltinEffect(EffectType::Saturation) {}

protected:
    void onPrepare(double sampleRate, uint32_t) override
    {
        drive_.prepare(sampleRate, 15.0);
        mix_.prepare(sampleRate, 15.0);
        output_.prepare(sampleRate, 15.0);
        reset();
    }
    void reset() override
    {
        drive_.reset(dbToLinear(value(1)));
        mix_.reset(value(2) * 0.01f);
        output_.reset(dbToLinear(value(3)));
    }
    void process(float *left, float *right, uint32_t frames) override
    {
        const int type = static_cast<int>(value(0) + 0.5f);
        const float drive = dbToLinear(value(1)), mix = value(2) * 0.01f, output = dbToLinear(value(3));
        for (uint32_t i = 0; i < frames; ++i) {
            const float d = drive_.next(drive), m = mix_.next(mix), o = output_.next(output);
            left[i] = left[i] * (1.0f - m) + shape(left[i] * d, type) * o * m;
            right[i] = right[i] * (1.0f - m) + shape(right[i] * d, type) * o * m;
        }
    }

private:
    static float shape(float x, int type)
    {
        if (type == 1)
            return std::clamp(x, -1.0f, 1.0f);
        if (type == 2)
            return std::sin(x * 1.5707963f);
        return std::tanh(x);
    }
    Smoothed drive_, mix_, output_;
};

// Delay: a feedback delay with a low-pass in the loop and an optional ping-pong
class DelayEffect : public BuiltinEffect {
public:
    DelayEffect() : BuiltinEffect(EffectType::Delay) {}

protected:
    void onPrepare(double sampleRate, uint32_t) override
    {
        size_ = static_cast<size_t>(sampleRate * 2.05) + 4;
        buffer_[0].assign(size_, 0.0f);
        buffer_[1].assign(size_, 0.0f);
        time_.prepare(sampleRate, 60.0);
        reset();
    }
    void reset() override
    {
        std::fill(buffer_[0].begin(), buffer_[0].end(), 0.0f);
        std::fill(buffer_[1].begin(), buffer_[1].end(), 0.0f);
        write_ = 0;
        tone_[0] = tone_[1] = 0.0f;
        time_.reset(static_cast<float>(value(0) * 0.001 * sampleRate_));
    }
    void process(float *left, float *right, uint32_t frames) override
    {
        const float targetDelay = static_cast<float>(value(0) * 0.001 * sampleRate_);
        const float feedback = value(1) * 0.01f, mix = value(4) * 0.01f;
        const float lowpass = static_cast<float>(std::exp(-2.0 * kPi * value(2) / sampleRate_));
        const bool pingPong = value(3) >= 0.5f;
        for (uint32_t i = 0; i < frames; ++i) {
            const float delay = std::clamp(time_.next(targetDelay), 1.0f, static_cast<float>(size_ - 2));
            float readPosition = static_cast<float>(write_) - delay;
            if (readPosition < 0.0f)
                readPosition += static_cast<float>(size_);
            const size_t a = static_cast<size_t>(readPosition) % size_, b = (a + 1) % size_;
            const float fraction = readPosition - std::floor(readPosition);
            float wet[2];
            for (int c = 0; c < 2; ++c)
                wet[c] = buffer_[c][a] + (buffer_[c][b] - buffer_[c][a]) * fraction;
            // The repeats, darkened a little each time round
            for (int c = 0; c < 2; ++c)
                tone_[c] = (1.0f - lowpass) * wet[c] + lowpass * tone_[c];
            const float inL = left[i], inR = right[i];
            if (pingPong) {
                buffer_[0][write_] = (inL + inR) * 0.5f + tone_[1] * feedback;
                buffer_[1][write_] = tone_[0] * feedback;
            } else {
                buffer_[0][write_] = inL + tone_[0] * feedback;
                buffer_[1][write_] = inR + tone_[1] * feedback;
            }
            if (++write_ >= size_)
                write_ = 0;
            left[i] = inL + wet[0] * mix;
            right[i] = inR + wet[1] * mix;
        }
    }

private:
    size_t size_ = 0, write_ = 0;
    std::vector<float> buffer_[2];
    float tone_[2] = {};
    Smoothed time_;
};

// Reverb: Freeverb, eight combs and four all-passes a side
class ReverbEffect : public BuiltinEffect {
public:
    ReverbEffect() : BuiltinEffect(EffectType::Reverb) {}

protected:
    void onPrepare(double sampleRate, uint32_t) override
    {
        static const int kComb[8] = {1116, 1188, 1277, 1356, 1422, 1491, 1557, 1617};
        static const int kAllpass[4] = {556, 441, 341, 225};
        const double scale = sampleRate / 44100.0;
        for (int c = 0; c < 2; ++c) {
            const int spread = c == 0 ? 0 : 23;
            for (int i = 0; i < 8; ++i)
                combs_[c][i].buffer.assign(static_cast<size_t>((kComb[i] + spread) * scale) + 1, 0.0f);
            for (int i = 0; i < 4; ++i)
                allpasses_[c][i].buffer.assign(static_cast<size_t>((kAllpass[i] + spread) * scale) + 1, 0.0f);
        }
        reset();
    }
    void reset() override
    {
        for (int c = 0; c < 2; ++c) {
            for (auto &comb : combs_[c]) {
                std::fill(comb.buffer.begin(), comb.buffer.end(), 0.0f);
                comb.index = 0;
                comb.store = 0.0f;
            }
            for (auto &allpass : allpasses_[c]) {
                std::fill(allpass.buffer.begin(), allpass.buffer.end(), 0.0f);
                allpass.index = 0;
            }
        }
    }
    void process(float *left, float *right, uint32_t frames) override
    {
        const float feedback = 0.7f + value(0) * 0.01f * 0.28f, damping = value(1) * 0.01f * 0.4f;
        const float width = value(2) * 0.01f, mix = value(3) * 0.01f;
        const float wet = mix * 3.0f, wet1 = wet * (width * 0.5f + 0.5f), wet2 = wet * ((1.0f - width) * 0.5f);
        const float dry = 1.0f - mix;
        for (uint32_t i = 0; i < frames; ++i) {
            const float in = (left[i] + right[i]) * 0.015f;
            float out[2] = {};
            for (int c = 0; c < 2; ++c) {
                for (auto &comb : combs_[c])
                    out[c] += comb.process(in, feedback, damping);
                for (auto &allpass : allpasses_[c])
                    out[c] = allpass.process(out[c]);
            }
            const float l = left[i], r = right[i];
            left[i] = l * dry + out[0] * wet1 + out[1] * wet2;
            right[i] = r * dry + out[1] * wet1 + out[0] * wet2;
        }
    }

private:
    struct Comb {
        std::vector<float> buffer;
        size_t index = 0;
        float store = 0.0f;
        float process(float in, float feedback, float damping)
        {
            const float out = buffer[index];
            store = out * (1.0f - damping) + store * damping;
            buffer[index] = in + store * feedback;
            if (++index >= buffer.size())
                index = 0;
            return out;
        }
    };
    struct Allpass {
        std::vector<float> buffer;
        size_t index = 0;
        float process(float in)
        {
            const float held = buffer[index];
            buffer[index] = in + held * 0.5f;
            if (++index >= buffer.size())
                index = 0;
            return -in + held;
        }
    };
    Comb combs_[2][8];
    Allpass allpasses_[2][4];
};

}

// Parameters

const EffectInfo &effectInfo(EffectType type)
{
    const int i = std::clamp(static_cast<int>(type), 0, kEffectTypeCount - 1);
    return kInfo[i];
}

bool effectFromKey(const std::string &key, EffectType &type)
{
    for (int i = 0; i < kEffectTypeCount; ++i) {
        if (key == kInfo[i].key) {
            type = static_cast<EffectType>(i);
            return true;
        }
    }
    return false;
}

DeviceSetting effectSetting(EffectType type)
{
    const EffectInfo &info = effectInfo(type);
    DeviceSetting s;
    s.kind = DeviceKind::Effect;
    s.id = info.key;
    s.name = info.name;
    for (size_t i = 0; i < info.paramCount; ++i)
        s.values.emplace_back(static_cast<uint32_t>(i), info.params[i].def);
    return s;
}

DeviceParam effectParam(const ParamInfo &param, uint32_t id)
{
    DeviceParam p;
    p.id = id;
    p.name = param.name;
    p.min = param.min;
    p.max = param.max;
    p.def = param.def;
    p.unit = param.unit;
    p.logarithmic = param.logarithmic;
    p.stepped = param.unit == ParamUnit::Choice || param.unit == ParamUnit::Toggle;
    if (param.unit == ParamUnit::Choice) {
        const std::string all = param.choices;
        size_t start = 0;
        for (;;) {
            const size_t end = all.find('|', start);
            p.choices.push_back(all.substr(start, end == std::string::npos ? std::string::npos : end - start));
            if (end == std::string::npos)
                break;
            start = end + 1;
        }
    }
    return p;
}

// BuiltinEffect

bool BuiltinEffect::matches(const DeviceSetting &setting) const
{
    EffectType type;
    return setting.kind == DeviceKind::Effect && effectFromKey(setting.id, type) && type == type_;
}

void BuiltinEffect::applySetting(const DeviceSetting &setting)
{
    setBypass(setting.bypass);
    const EffectInfo &info = effectInfo(type_);
    for (size_t p = 0; p < info.paramCount; ++p)
        setParam(p, static_cast<float>(setting.value(static_cast<uint32_t>(p), info.params[p].def)));
}

std::vector<DeviceParam> BuiltinEffect::params() const
{
    const EffectInfo &info = effectInfo(type_);
    std::vector<DeviceParam> out;
    for (size_t p = 0; p < info.paramCount; ++p)
        out.push_back(effectParam(info.params[p], static_cast<uint32_t>(p)));
    return out;
}

void BuiltinEffect::setParam(size_t index, float value)
{
    const EffectInfo &info = effectInfo(type_);
    if (index >= info.paramCount)
        return;
    values_[index].store(std::clamp(value, info.params[index].min, info.params[index].max), std::memory_order_relaxed);
}

// Effect

void Effect::prepare(double sampleRate, uint32_t maxFrames)
{
    sampleRate_ = sampleRate;
    dry_[0].assign(maxFrames, 0.0f);
    dry_[1].assign(maxFrames, 0.0f);
    wetStep_ = static_cast<float>(1.0 / (0.01 * sampleRate));
    wet_ = bypassed() ? 0.0f : 1.0f;
    onPrepare(sampleRate, maxFrames);
    reset();
    fresh_ = true;
}

void Effect::run(float *left, float *right, uint32_t frames, const TransportState *transport)
{
    transport_ = transport;
    if (fresh_) {
        // The parameters were set after prepare: start from them, not the defaults
        reset();
        fresh_ = false;
    }
    const bool bypass = bypassed();
    const float target = bypass ? 0.0f : 1.0f;
    if (wet_ == 0.0f && bypass) {
        setMeter(0.0f);
        return;
    }
    if (wet_ == 0.0f)
        reset(); // coming back from bypass: no stale tail
    if (wet_ == 1.0f && !bypass) {
        process(left, right, frames);
        return;
    }
    if (frames > dry_[0].size()) {
        process(left, right, frames);
        wet_ = target;
        return;
    }
    std::memcpy(dry_[0].data(), left, frames * sizeof(float));
    std::memcpy(dry_[1].data(), right, frames * sizeof(float));
    process(left, right, frames);
    for (uint32_t i = 0; i < frames; ++i) {
        wet_ = wet_ < target ? std::min(target, wet_ + wetStep_) : std::max(target, wet_ - wetStep_);
        left[i] = dry_[0][i] + (left[i] - dry_[0][i]) * wet_;
        right[i] = dry_[1][i] + (right[i] - dry_[1][i]) * wet_;
    }
}

std::unique_ptr<BuiltinEffect> makeEffect(EffectType type)
{
    switch (type) {
    case EffectType::Gain: return std::make_unique<GainEffect>();
    case EffectType::Equalizer: return std::make_unique<EqEffect>();
    case EffectType::Filter: return std::make_unique<FilterEffect>();
    case EffectType::Gate: return std::make_unique<GateEffect>();
    case EffectType::Compressor: return std::make_unique<CompressorEffect>();
    case EffectType::Limiter: return std::make_unique<LimiterEffect>();
    case EffectType::Saturation: return std::make_unique<SaturationEffect>();
    case EffectType::Delay: return std::make_unique<DelayEffect>();
    case EffectType::Reverb: return std::make_unique<ReverbEffect>();
    }
    return std::make_unique<GainEffect>();
}

}
