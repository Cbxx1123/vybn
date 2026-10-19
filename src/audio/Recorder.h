#pragma once

#include <atomic>
#include <cstdint>
#include <vector>

#include "AudioProcessor.h"

namespace audio {

// Captures the audio input while armed and the transport plays: every input channel, from the
// first block in which both hold. The audio thread writes into a lock-free ring buffer (about
// 20 seconds of stereo); the main thread drains it every frame into the take it is building.
// If the main thread falls behind (a modal system dialog, say), frames that don't fit are
// dropped and counted. The ring (8 MB) exists only from arm() until releaseMemory(), so a session
// that never records never pays for it.
class InputRecorder : public AudioProcessor {
public:
    InputRecorder();

    // Main thread
    void arm();    // the take starts at the next block where the transport plays with input (allocates the ring)
    void disarm(); // the take ends after the current block
    bool armed() const { return armed_.load(std::memory_order_relaxed); }
    // Whether the audio thread is still writing (it stops a block after disarm)
    bool capturing() const { return capturing_.load(std::memory_order_acquire); }
    // Whether the take has begun, and where: the transport position of its first frame and its
    // channel count
    bool started() const { return started_.load(std::memory_order_acquire); }
    uint64_t startPosition() const { return start_.load(std::memory_order_relaxed); }
    uint32_t channels() const { return channels_.load(std::memory_order_relaxed); }
    uint64_t dropped() const { return dropped_.load(std::memory_order_relaxed); }
    // Appends what has arrived (interleaved, channels() per frame) and returns the frames added
    size_t drain(std::vector<float> &out);
    // Frees the ring once a take is over (disarmed, drained) and the audio thread is out of it;
    // returns whether it did. The next arm() makes it again.
    bool releaseMemory();

    void process(AudioBlock &block) override;

private:
    void capture(AudioBlock &block);

    static constexpr size_t kRing = size_t(1) << 21; // floats; a power of two
    std::vector<float> ring_;
    std::atomic<size_t> write_{0}; // audio thread
    std::atomic<size_t> read_{0};  // main thread
    std::atomic<bool> armed_{false};
    // Set while process() might touch the ring. With armed_, both sequentially consistent: the
    // main thread clears armed_ then reads this, the audio thread sets this then reads armed_, so
    // one of them always sees the other (the ring is never freed under a write).
    std::atomic<bool> inRing_{false};
    std::atomic<bool> capturing_{false};
    std::atomic<bool> started_{false};
    std::atomic<uint64_t> start_{0};
    std::atomic<uint32_t> channels_{0};
    std::atomic<uint64_t> dropped_{0};
};

}
