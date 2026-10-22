#pragma once

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <memory>
#include <vector>

#include "AudioProcessor.h"
#include "TempoMap.h"

namespace audio {

// Play state, position and tempo map of the timeline. The main thread asks for changes; the audio
// thread applies them at the start of each block and moves the position while playing. Every
// call is lock-free, so it is safe from either thread, except setTiming (main thread only).
// The tempo map: the main thread publishes an immutable copy, which the audio thread takes at the
// start of a block, keeping the playhead on the same beat (so a tempo edit while playing doesn't
// jump), and the old copy is freed on the main thread once no block can be using it.
// The loop: while `looping` and the range is set, playing past its end jumps back to its start,
// on the exact sample (the AudioSystem ends a block there: `framesBeforeLoop`). Its range is in
// beats, so it stays put in the music when the tempo changes.
class Transport {
public:
    Transport() { setTiming(TempoMap()); }

    // Main thread (or any)
    void play() { playing_.store(true, std::memory_order_relaxed); }
    void stop() { playing_.store(false, std::memory_order_relaxed); }
    void toggle() { playing_.store(!playing(), std::memory_order_relaxed); }
    // Moves the position, at the start of the next block
    void locate(uint64_t sample)
    {
        locateTarget_.store(sample, std::memory_order_relaxed);
        locatePending_.store(true, std::memory_order_release);
    }
    // The loop range in beats (end after start), and whether playback loops in it
    void setLoop(double start, double end)
    {
        loopStart_.store(std::max(0.0, start), std::memory_order_relaxed);
        loopEnd_.store(std::max(end, start), std::memory_order_relaxed);
    }
    void setLooping(bool on) { looping_.store(on, std::memory_order_relaxed); }
    bool looping() const { return looping_.load(std::memory_order_relaxed); }
    double loopStart() const { return loopStart_.load(std::memory_order_relaxed); }
    double loopEnd() const { return loopEnd_.load(std::memory_order_relaxed); }

    bool playing() const { return playing_.load(std::memory_order_relaxed); }
    uint64_t position() const { return position_.load(std::memory_order_relaxed); }

    // Main thread: the tempo map, published when it differs from the last; a constant tempo is
    // a map with one point
    void setTiming(const TempoMap &timing)
    {
        if (current_ && *current_ == timing)
            return;
        const uint64_t blocks = blocks_.load(std::memory_order_acquire);
        retired_.erase(std::remove_if(retired_.begin(), retired_.end(), [&](const Retired &r) { return blocks >= r.stamp + 2; }),
                       retired_.end());
        auto fresh = std::make_unique<const TempoMap>(timing);
        published_.store(fresh.get(), std::memory_order_release);
        if (current_)
            retired_.push_back({std::move(current_), blocks});
        current_ = std::move(fresh);
    }
    void setTempo(double bpm) { setTiming(TempoMap(bpm)); }
    // Main thread: the map last published (the audio thread takes it at its next block)
    const TempoMap &timing() const { return *current_; }
    // Main thread: the position in beats through that map, and a beat as a position
    double beats(double sampleRate) const { return timing().beatAtSample(static_cast<double>(position()), sampleRate); }
    uint64_t sampleOf(double beat, double sampleRate) const
    {
        return static_cast<uint64_t>(std::llround(std::max(0.0, timing().samplesAt(beat, sampleRate))));
    }

    // Audio thread: the state for the block about to be processed
    TransportState begin(double sampleRate)
    {
        blocks_.fetch_add(1, std::memory_order_release);
        const bool located = locatePending_.exchange(false, std::memory_order_acquire);
        if (located)
            position_.store(locateTarget_.load(std::memory_order_relaxed), std::memory_order_relaxed);
        const TempoMap *next = published_.load(std::memory_order_acquire);
        if (next != map_) {
            // The playhead keeps its beat in the new map
            if (map_ && !located) {
                const double beat = map_->beatAtSample(static_cast<double>(position()), sampleRate);
                position_.store(static_cast<uint64_t>(std::max(0.0, std::round(next->samplesAt(beat, sampleRate)))),
                                std::memory_order_relaxed);
            }
            map_ = next;
        }
        TransportState state;
        state.playing = playing();
        state.position = position();
        state.sampleRate = sampleRate;
        state.map = map_;
        state.tempo = map_->bpmAt(state.beatAt(0));
        return state;
    }
    // Audio thread: how many of `frames` to render before the loop's end, where the block must
    // stop so the next one starts back at the loop's start
    uint32_t framesBeforeLoop(const TransportState &state, uint32_t frames) const
    {
        uint64_t start = 0, end = 0;
        if (!state.playing || !loopSamples(state, start, end) || state.position >= end)
            return frames;
        return static_cast<uint32_t>(std::min<uint64_t>(frames, end - state.position));
    }
    // Audio thread: after a block of `frames`
    void advance(const TransportState &state, uint32_t frames)
    {
        if (!state.playing)
            return;
        uint64_t next = state.position + frames;
        uint64_t start = 0, end = 0;
        if (loopSamples(state, start, end) && state.position < end && next >= end)
            next = start;
        position_.store(next, std::memory_order_relaxed);
    }

private:
    // The loop's range in samples through the block's map, when looping over a range
    bool loopSamples(const TransportState &state, uint64_t &start, uint64_t &end) const
    {
        const double a = loopStart(), b = loopEnd();
        if (!looping() || b <= a)
            return false;
        start = static_cast<uint64_t>(std::llround(state.map->samplesAt(a, state.sampleRate)));
        end = static_cast<uint64_t>(std::llround(state.map->samplesAt(b, state.sampleRate)));
        return end > start;
    }

    struct Retired {
        std::unique_ptr<const TempoMap> map;
        uint64_t stamp;
    };

    std::atomic<bool> playing_{false};
    std::atomic<uint64_t> position_{0};
    std::atomic<uint64_t> locateTarget_{0};
    std::atomic<bool> locatePending_{false};
    std::atomic<bool> looping_{false};
    std::atomic<double> loopStart_{0.0};
    std::atomic<double> loopEnd_{0.0};
    std::unique_ptr<const TempoMap> current_; // main thread
    std::vector<Retired> retired_;            // main thread
    std::atomic<const TempoMap *> published_{nullptr};
    std::atomic<uint64_t> blocks_{0};  // blocks begun
    const TempoMap *map_ = nullptr;    // audio thread: the map in use
};

}
