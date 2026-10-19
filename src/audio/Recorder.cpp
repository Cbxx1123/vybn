#include "Recorder.h"

#include <algorithm>

namespace audio {

InputRecorder::InputRecorder() : AudioProcessor("Recorder") {}

void InputRecorder::arm()
{
    // Only between takes: the audio thread isn't writing, so the ring can be emptied
    if (capturing())
        return;
    // Disarmed, so the audio thread doesn't touch the ring: it can be made here. Arming
    // publishes it.
    if (ring_.empty())
        ring_.assign(kRing, 0.0f);
    read_.store(write_.load(std::memory_order_acquire), std::memory_order_release);
    started_.store(false, std::memory_order_release);
    dropped_.store(0, std::memory_order_relaxed);
    armed_.store(true, std::memory_order_release);
}

void InputRecorder::disarm()
{
    armed_.store(false, std::memory_order_seq_cst);
}

bool InputRecorder::releaseMemory()
{
    if (ring_.empty())
        return true;
    if (armed_.load(std::memory_order_seq_cst) || capturing() || inRing_.load(std::memory_order_seq_cst))
        return false;
    if (read_.load(std::memory_order_relaxed) != write_.load(std::memory_order_acquire))
        return false; // not drained yet
    std::vector<float>().swap(ring_);
    return true;
}

size_t InputRecorder::drain(std::vector<float> &out)
{
    const uint32_t ch = channels();
    if (ch == 0)
        return 0;
    const size_t r = read_.load(std::memory_order_relaxed);
    const size_t w = write_.load(std::memory_order_acquire);
    const size_t available = (w - r) / ch * ch; // whole frames
    for (size_t i = 0; i < available; ++i)
        out.push_back(ring_[(r + i) & (kRing - 1)]);
    read_.store(r + available, std::memory_order_release);
    return available / ch;
}

void InputRecorder::process(AudioBlock &block)
{
    inRing_.store(true, std::memory_order_seq_cst);
    capture(block);
    inRing_.store(false, std::memory_order_release);
}

void InputRecorder::capture(AudioBlock &block)
{
    const TransportState &t = block.transport;
    const bool want = armed_.load(std::memory_order_seq_cst) && t.playing && block.inputs && block.inputCount > 0;
    if (!want) {
        // A take ends when it is disarmed, the transport stops or the input goes
        if (capturing_.load(std::memory_order_relaxed))
            capturing_.store(false, std::memory_order_release);
        return;
    }
    if (!capturing_.load(std::memory_order_relaxed)) {
        if (started_.load(std::memory_order_relaxed))
            return; // this take already ended; the next arm starts another
        channels_.store(block.inputCount, std::memory_order_relaxed);
        start_.store(t.position, std::memory_order_relaxed);
        started_.store(true, std::memory_order_release);
        capturing_.store(true, std::memory_order_release);
    }
    const uint32_t ch = channels_.load(std::memory_order_relaxed);
    if (ch != block.inputCount) {
        capturing_.store(false, std::memory_order_release); // the input changed under the take
        return;
    }
    const size_t w = write_.load(std::memory_order_relaxed);
    const size_t r = read_.load(std::memory_order_acquire);
    const size_t needed = static_cast<size_t>(block.frames) * ch;
    if (kRing - (w - r) < needed) {
        dropped_.fetch_add(block.frames, std::memory_order_relaxed);
        return;
    }
    for (uint32_t i = 0; i < block.frames; ++i) {
        for (uint32_t c = 0; c < ch; ++c)
            ring_[(w + static_cast<size_t>(i) * ch + c) & (kRing - 1)] = block.inputs[c][i];
    }
    write_.store(w + needed, std::memory_order_release);
}

}
