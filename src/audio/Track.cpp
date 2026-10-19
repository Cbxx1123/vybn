#include "Track.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace audio {

namespace {

constexpr float kQuarterPi = 0.7853981633974483f;

}

TrackProcessor::TrackProcessor(std::string name, std::shared_ptr<SoloGroup> group, std::unique_ptr<AudioProcessor> source)
    : AudioProcessor(std::move(name)), group_(std::move(group)), source_(std::move(source))
{
}

TrackProcessor::~TrackProcessor()
{
    // Deleted on the main thread once the audio thread has let go, so it's out of the mix
    if (group_ && solo_.load(std::memory_order_relaxed))
        group_->soloed_.fetch_sub(1, std::memory_order_relaxed);
}

void TrackProcessor::setSolo(bool on)
{
    if (solo_.exchange(on, std::memory_order_relaxed) != on && group_)
        group_->soloed_.fetch_add(on ? 1 : -1, std::memory_order_relaxed);
}

bool TrackProcessor::audible() const
{
    if (mute.load(std::memory_order_relaxed))
        return false;
    return solo() || !group_ || group_->soloed() == 0;
}

float TrackProcessor::takePeak(uint32_t channel)
{
    return channel < kChannels ? peaks_[channel].exchange(0.0f, std::memory_order_relaxed) : 0.0f;
}

void TrackProcessor::setTap(float *left, float *right, bool preFader)
{
    tapLeft_ = left && right ? left : nullptr;
    tapRight_ = left && right ? right : nullptr;
    tapPreFader_ = preFader;
}

void TrackProcessor::prepare(double sampleRate, uint32_t maxFrames)
{
    for (auto &bus : bus_)
        bus.assign(maxFrames, 0.0f);
    for (auto &level : level_) {
        level.prepare(sampleRate);
        level.reset(0.0f);
    }
    if (source_)
        source_->prepare(sampleRate, maxFrames);
    chain.prepare(sampleRate, maxFrames);
    for (auto &line : delay_)
        line.assign(kDelaySize, 0.0f);
    delayWrite_ = 0;
}

std::vector<uint32_t> compensations(const std::vector<uint32_t> &latencies)
{
    uint32_t most = 0;
    for (uint32_t l : latencies)
        most = std::max(most, std::min(l, TrackProcessor::kMaxCompensation));
    std::vector<uint32_t> out;
    for (uint32_t l : latencies)
        out.push_back(most - std::min(l, most));
    return out;
}

void TrackProcessor::process(AudioBlock &block)
{
    renderParallel(block);
    mixInto(block);
}

void TrackProcessor::renderParallel(const AudioBlock &block)
{
    const uint32_t frames = std::min<uint32_t>(block.frames, static_cast<uint32_t>(bus_[0].size()));
    rendered_ = frames;
    for (auto &bus : bus_)
        std::fill(bus.begin(), bus.begin() + frames, 0.0f);

    if (source_) {
        float *channels[kChannels] = {bus_[0].data(), bus_[1].data()};
        AudioBlock sourceBlock{channels, kChannels, frames, block.sampleRate, block.transport, block.inputs, block.inputCount};
        source_->process(sourceBlock);
    }
    chain.process(bus_[0].data(), bus_[1].data(), frames, &block.transport);

    // Delay compensation: through the delay line always, so its history is there when it changes
    if (!delay_[0].empty()) {
        const uint32_t delay = std::min(compensation.load(std::memory_order_relaxed), kMaxCompensation);
        constexpr uint32_t mask = kDelaySize - 1;
        for (uint32_t c = 0; c < kChannels; ++c) {
            float *bus = bus_[c].data();
            float *line = delay_[c].data();
            for (uint32_t i = 0; i < frames; ++i) {
                const uint32_t at = delayWrite_ + i;
                line[at & mask] = bus[i];
                bus[i] = line[(at - delay) & mask];
            }
        }
        delayWrite_ = (delayWrite_ + frames) & mask;
    }

    // The tap before the gain, pan and mute (offline engines)
    const bool tapped = tapLeft_ != nullptr;
    if (tapped && tapPreFader_) {
        std::copy_n(bus_[0].data(), frames, tapLeft_);
        std::copy_n(bus_[1].data(), frames, tapRight_);
    }

    // Targets for this block; the levels glide toward them a sample at a time
    float target[kChannels] = {};
    if (audible()) {
        const float angle = (pan.get() + 1.0f) * kQuarterPi;
        const float g = gain.get();
        target[0] = g * std::cos(angle);
        target[1] = g * std::sin(angle);
    }

    // Gain and pan in place, metered
    for (uint32_t c = 0; c < kChannels; ++c) {
        float peak = 0.0f;
        float *bus = bus_[c].data();
        for (uint32_t i = 0; i < frames; ++i) {
            bus[i] *= level_[c].next(target[c]);
            peak = std::max(peak, std::fabs(bus[i]));
        }
        // Peaks only grow until the reader takes them
        float previous = peaks_[c].load(std::memory_order_relaxed);
        while (peak > previous && !peaks_[c].compare_exchange_weak(previous, peak, std::memory_order_relaxed)) {
        }
    }

    // The tap after them: what the track adds into the mix
    if (tapped && !tapPreFader_) {
        std::copy_n(bus_[0].data(), frames, tapLeft_);
        std::copy_n(bus_[1].data(), frames, tapRight_);
    }
}

void TrackProcessor::mixInto(AudioBlock &block)
{
    // A mono output gets the left channel
    const uint32_t outputs = std::min(block.channelCount, kChannels);
    const uint32_t frames = std::min(rendered_, block.frames);
    for (uint32_t c = 0; c < outputs; ++c) {
        const float *bus = bus_[c].data();
        float *out = block.channels[c];
        for (uint32_t i = 0; i < frames; ++i)
            out[i] += bus[i];
    }
}

}
