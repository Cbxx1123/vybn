#pragma once

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <string>
#include <utility>

#include "audio/TempoMap.h"

namespace audio {

// The transport as the audio thread sees it at the start of a block
struct TransportState {
    bool playing = false;
    uint64_t position = 0; // samples since the start of the timeline
    double tempo = 120.0;  // beats per minute at the block's start
    double sampleRate = 48000.0;
    const TempoMap *map = nullptr; // the tempo map; none: `tempo` throughout

    // Beats since the start of the timeline at `frame` frames into the block
    double beatAt(double frame) const
    {
        const double sample = static_cast<double>(position) + frame;
        return map ? map->beatAtSample(sample, sampleRate) : sample * tempo / (sampleRate * 60.0);
    }
    // Frames from the block's start to `beat` (negative before it)
    double frameAt(double beat) const
    {
        const double sample = map ? map->samplesAt(beat, sampleRate) : beat * sampleRate * 60.0 / tempo;
        return sample - static_cast<double>(position);
    }
    // Seconds from the start of the timeline to `beat`
    double secondsAt(double beat) const { return map ? map->secondsAt(beat) : beat * 60.0 / tempo; }
};

// One block of audio to produce: planar channels (one float array each). Processors add their
// output into the channels, which arrive holding what earlier processors wrote. `inputs` is the
// audio input's channels for the same frames (planar, read-only), when an input is open.
struct AudioBlock {
    float *const *channels;
    uint32_t channelCount;
    uint32_t frames;
    double sampleRate;
    const TransportState &transport;
    const float *const *inputs = nullptr;
    uint32_t inputCount = 0;
};

// A unit of the audio graph. For now the AudioSystem mixes every processor into the master bus;
// later these become tracks, instruments and effects.
//
// Threads:
// - prepare runs on the main thread, while this processor is not being processed: before it
//   joins the graph and whenever the device's sample rate or block size changes.
// - process runs on the audio thread. It must not lock, allocate, free, or do I/O, and must
//   finish well within the block's duration. Talk to it through AudioParams and atomics.
class AudioProcessor {
public:
    explicit AudioProcessor(std::string name) : name_(std::move(name)) {}
    virtual ~AudioProcessor() = default;

    AudioProcessor(const AudioProcessor &) = delete;
    AudioProcessor &operator=(const AudioProcessor &) = delete;

    const std::string &name() const { return name_; }

    virtual void prepare(double /*sampleRate*/, uint32_t /*maxFrames*/) {}
    virtual void process(AudioBlock &block) = 0;

    // Processors that render into buffers of their own, touching nothing another processor
    // touches (tracks), say so here. The AudioSystem then renders them all at once across the
    // real-time pool's threads (renderParallel, on any of them) and adds each into the mix in
    // graph order (mixInto, on the audio thread), after every other processor has run process()
    // in order. process() must still do both, for anything that calls it directly.
    virtual bool parallel() const { return false; }
    virtual void renderParallel(const AudioBlock & /*block*/) {}
    virtual void mixInto(AudioBlock & /*block*/) {}

private:
    std::string name_;
};

// A value set from any thread (a UI knob) and read on the audio thread. Lock-free; the value
// is clamped to its range.
class AudioParam {
public:
    AudioParam(float initial, float min, float max) : min_(min), max_(max), value_(std::clamp(initial, min, max)) {}

    void set(float value) { value_.store(std::clamp(value, min_, max_), std::memory_order_relaxed); }
    float get() const { return value_.load(std::memory_order_relaxed); }
    float min() const { return min_; }
    float max() const { return max_; }
    // As a 0-1 position in the range, for sliders
    float normalized() const { return max_ > min_ ? (get() - min_) / (max_ - min_) : 0.0f; }
    void setNormalized(float t) { set(min_ + std::clamp(t, 0.0f, 1.0f) * (max_ - min_)); }

private:
    float min_;
    float max_;
    std::atomic<float> value_;
};

// Glides toward a target a sample at a time, so parameter changes don't click. Audio thread.
class Smoothed {
public:
    // `ms` is roughly how long a change takes to settle
    void prepare(double sampleRate, double ms = 20.0)
    {
        coefficient_ = static_cast<float>(1.0 - std::exp(-1.0 / (sampleRate * ms * 0.001)));
    }
    void reset(float value) { current_ = value; }
    float next(float target)
    {
        current_ += (target - current_) * coefficient_;
        return current_;
    }
    float current() const { return current_; }

private:
    float current_ = 0.0f;
    float coefficient_ = 1.0f;
};

// Amplitude to decibels and back
inline float toDecibels(float amplitude)
{
    return amplitude > 1e-6f ? 20.0f * std::log10(amplitude) : -120.0f;
}

inline float fromDecibels(float db)
{
    return std::pow(10.0f, db / 20.0f);
}

}
