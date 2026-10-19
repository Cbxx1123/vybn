#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "AudioProcessor.h"
#include "DeviceChain.h"

namespace audio {

// The tracks that share a solo: while any of them is soloed, only the soloed ones are heard.
// Shared by its tracks (each holds it), so it lives as long as the last of them.
class SoloGroup {
public:
    int soloed() const { return soloed_.load(std::memory_order_relaxed); }

private:
    friend class TrackProcessor;
    std::atomic<int> soloed_{0};
};

// One mixer channel: renders its source into its own stereo bus, applies gain, pan, mute and
// solo, meters the result and adds it into the block. Every parameter is an AudioParam or an
// atomic, set from any thread; changes glide, so they don't click.
//
// Pan is constant power with a -3 dB centre: left gain cos(a), right gain sin(a), with a from 0
// (hard left) to pi/2 (hard right). A source's left and right channels are scaled by those, so
// a mono source (the same signal on both) pans and a stereo one is balanced.
class TrackProcessor : public AudioProcessor {
public:
    static constexpr uint32_t kChannels = 2;

    // `source` makes the sound the track mixes (nullptr for a silent track, as every DAW track
    // is until clips hold audio). The track owns it and prepares and processes it; it can't be
    // changed once the track is in the graph.
    TrackProcessor(std::string name, std::shared_ptr<SoloGroup> group, std::unique_ptr<AudioProcessor> source = nullptr);
    ~TrackProcessor() override;

    AudioParam gain{1.0f, 0.0f, 2.0f}; // linear, up to +6 dB
    AudioParam pan{0.0f, -1.0f, 1.0f}; // -1 hard left, +1 hard right
    std::atomic<bool> mute{false};

    // Solo goes through the group, so set it here (any thread)
    void setSolo(bool on);
    bool solo() const { return solo_.load(std::memory_order_relaxed); }
    // Whether the track is heard now: not muted, and soloed or nothing in its group is
    bool audible() const;

    AudioProcessor *source() { return source_.get(); }

    // The track's devices: its instrument (which an instrument track's source, a NotePlayer with a
    // ChainInstrument, plays) and its effects, between the source and the gain and pan. Set with
    // chain.apply(), free what it replaced with chain.collect() (main thread).
    DeviceChain chain;

    // Delay compensation: how many frames the track's sound (after its effects) is held back, so
    // it lines up with tracks whose plugins delay theirs more (any thread; see compensations()).
    // The delay line is made in prepare, so changing it never allocates.
    static constexpr uint32_t kMaxCompensation = 16384;
    std::atomic<uint32_t> compensation{0};

    // The highest absolute sample on a channel after gain and pan since the last call (one
    // reader: it resets the value)
    float takePeak(uint32_t channel);

    // Offline engines only: copies every block's frames into `left` and `right` (each at least
    // AudioSystem::kMaxBlock long), before the gain, pan and mute when preFader, else after them,
    // which is exactly what the track adds into the mix (silence while it is muted). Set on the
    // main thread while the track isn't processing; nullptr for no tap. The live app never sets one.
    void setTap(float *left, float *right, bool preFader);

    void prepare(double sampleRate, uint32_t maxFrames) override;
    void process(AudioBlock &block) override;
    // Tracks render in parallel: the source, gain and pan into the track's own bus, then the bus
    // is added into the mix
    bool parallel() const override { return true; }
    void renderParallel(const AudioBlock &block) override;
    void mixInto(AudioBlock &block) override;

private:
    std::shared_ptr<SoloGroup> group_;
    std::unique_ptr<AudioProcessor> source_;
    std::atomic<bool> solo_{false};

    // Audio thread
    std::array<std::vector<float>, kChannels> bus_; // sized in prepare
    std::array<Smoothed, kChannels> level_;         // gain times pan, per channel
    std::array<std::atomic<float>, kChannels> peaks_{};
    uint32_t rendered_ = 0; // frames in the bus from the last renderParallel
    float *tapLeft_ = nullptr;
    float *tapRight_ = nullptr;
    bool tapPreFader_ = false;
    // The compensation's delay line: a ring per channel, written every block
    static constexpr uint32_t kDelaySize = 32768; // a power of two, above kMaxCompensation + a block
    std::array<std::vector<float>, kChannels> delay_;
    uint32_t delayWrite_ = 0;
};

// Delay compensation for tracks whose sound is delayed by `latencies` frames (their plugins'): how
// much each must be held back so all line up with the latest, each at most kMaxCompensation
std::vector<uint32_t> compensations(const std::vector<uint32_t> &latencies);

}
