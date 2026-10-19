#pragma once

#include <algorithm>
#include <atomic>
#include <cstdint>

#include "AudioProcessor.h"

namespace audio {

// Play state, position and tempo of the timeline. The main thread asks for changes; the audio
// thread applies them at the start of each block and moves the position while playing. Every
// call is lock-free, so it is safe from either thread.
// The loop: while `looping` and the range is set, playing past its end jumps back to its start,
// on the exact sample (the AudioSystem ends a block there: `framesBeforeLoop`).
class Transport {
public:
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
    void setTempo(double bpm) { tempo_.store(std::clamp(bpm, 20.0, 400.0), std::memory_order_relaxed); }
    // The loop range in samples (end after start), and whether playback loops in it
    void setLoop(uint64_t start, uint64_t end)
    {
        loopStart_.store(start, std::memory_order_relaxed);
        loopEnd_.store(std::max(end, start), std::memory_order_relaxed);
    }
    void setLooping(bool on) { looping_.store(on, std::memory_order_relaxed); }
    bool looping() const { return looping_.load(std::memory_order_relaxed); }
    uint64_t loopStart() const { return loopStart_.load(std::memory_order_relaxed); }
    uint64_t loopEnd() const { return loopEnd_.load(std::memory_order_relaxed); }

    bool playing() const { return playing_.load(std::memory_order_relaxed); }
    double tempo() const { return tempo_.load(std::memory_order_relaxed); }
    uint64_t position() const { return position_.load(std::memory_order_relaxed); }

    // Audio thread: the state for the block about to be processed
    TransportState begin(double sampleRate)
    {
        if (locatePending_.exchange(false, std::memory_order_acquire))
            position_.store(locateTarget_.load(std::memory_order_relaxed), std::memory_order_relaxed);
        TransportState state;
        state.playing = playing();
        state.position = position();
        state.tempo = tempo();
        state.sampleRate = sampleRate;
        return state;
    }
    // Audio thread: how many of `frames` to render before the loop's end, where the block must
    // stop so the next one starts back at the loop's start
    uint32_t framesBeforeLoop(const TransportState &state, uint32_t frames) const
    {
        const uint64_t start = loopStart(), end = loopEnd();
        if (!state.playing || !looping() || end <= start || state.position >= end)
            return frames;
        return static_cast<uint32_t>(std::min<uint64_t>(frames, end - state.position));
    }
    // Audio thread: after a block of `frames`
    void advance(const TransportState &state, uint32_t frames)
    {
        if (!state.playing)
            return;
        uint64_t next = state.position + frames;
        const uint64_t start = loopStart(), end = loopEnd();
        if (looping() && end > start && state.position < end && next >= end)
            next = start;
        position_.store(next, std::memory_order_relaxed);
    }

private:
    std::atomic<bool> playing_{false};
    std::atomic<double> tempo_{120.0};
    std::atomic<uint64_t> position_{0};
    std::atomic<uint64_t> locateTarget_{0};
    std::atomic<bool> locatePending_{false};
    std::atomic<bool> looping_{false};
    std::atomic<uint64_t> loopStart_{0};
    std::atomic<uint64_t> loopEnd_{0};
};

}
