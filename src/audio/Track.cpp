#include "Track.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace audio {

namespace {

constexpr float kQuarterPi = 0.7853981633974483f;

// How near a gliding level must be to its target to count as there (and be put there): Smoothed moves by
// a small share of the gap each sample, so in floats it stops short, some 1e-5 away, and never gets
// nearer. 1e-4 is -80 dB of a full level, so the snap can't be heard, and the steady paths are taken.
constexpr float kSettled = 1e-4f;

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
    blockStart_ = 0;
    for (auto &ramp : ramp_)
        ramp.assign(maxFrames, 0.0f);
    pre_.assign(maxFrames, 0.0f);
    scratch_.assign(maxFrames, 0.0f);
    for (auto &key : key_) {
        for (auto &channel : key)
            channel.assign(maxFrames, 0.0f);
    }
    preLevel_.prepare(sampleRate);
    preLevel_.reset(0.0f);
    preGliding_ = false;
    for (Smoothed &send : sendSmooth_) {
        send.prepare(sampleRate);
        send.reset(0.0f);
    }
}

void TrackProcessor::setSendLevel(uint32_t slot, float level)
{
    if (slot < kMaxSends)
        sendLevels_[slot].store(std::clamp(level, 0.0f, 1.0f), std::memory_order_relaxed);
}

float TrackProcessor::sendLevel(uint32_t slot) const
{
    return slot < kMaxSends ? sendLevels_[slot].load(std::memory_order_relaxed) : 0.0f;
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
    render(block, nullptr);
}

void TrackProcessor::renderRouted(const AudioBlock &block, const Route &route)
{
    render(block, &route);
}

void TrackProcessor::render(const AudioBlock &block, const Route *route)
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
    // What feeds it (a bus): rendered in the stages before, taken in before its effects
    if (route) {
        for (const Feed &feed : route->feeds)
            pull(feed, block.transport, frames);
        // Sidechain keys: the sources rendered in earlier stages, so their delay lines hold still
        uint32_t k = 0;
        for (const KeyFeed &key : route->keys) {
            if (k >= kMaxKeys)
                break;
            if (!key.from || key.from == this)
                continue;
            key.from->readKey(key.delay, frames, key_[k][0].data(), key_[k][1].data());
            chain.setKey(key.effect, key_[k][0].data(), key_[k][1].data());
            ++k;
        }
    }
    chain.process(bus_[0].data(), bus_[1].data(), frames, &block.transport);

    // Delay compensation: through the delay line always, so its history is there when it changes
    // (and sends read it at delays of their own)
    blockStart_ = delayWrite_;
    if (!delay_[0].empty()) {
        const uint32_t delay = std::min(compensation.load(std::memory_order_relaxed), kMaxCompensation);
        outputDelay_ = delay;
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
    const bool heard = audible();
    if (heard) {
        // The track's own automation, where it has some, in place of its fader and pan
        float g = gain.get(), p = pan.get();
        chain.automateMix(block.transport, g, p);
        g = std::clamp(g, gain.min(), gain.max());
        p = std::clamp(p, pan.min(), pan.max());
        const float angle = (p + 1.0f) * kQuarterPi;
        target[0] = g * std::cos(angle);
        target[1] = g * std::sin(angle);
    }

    // Gain and pan in place, metered; the levels kept for post-fader sends at other delays
    for (uint32_t c = 0; c < kChannels; ++c) {
        float peak = 0.0f;
        float *bus = bus_[c].data();
        float *ramp = ramp_[c].data();
        for (uint32_t i = 0; i < frames; ++i) {
            ramp[i] = level_[c].next(target[c]);
            bus[i] *= ramp[i];
            peak = std::max(peak, std::fabs(bus[i]));
        }
        // Peaks only grow until the reader takes them
        float previous = peaks_[c].load(std::memory_order_relaxed);
        while (peak > previous && !peaks_[c].compare_exchange_weak(previous, peak, std::memory_order_relaxed)) {
        }
    }

    // The mute alone, for pre-fader sends: a sample at a time only while it glides
    const float preTarget = heard ? 1.0f : 0.0f;
    preGliding_ = std::fabs(preLevel_.current() - preTarget) > kSettled;
    if (preGliding_) {
        for (uint32_t i = 0; i < frames; ++i)
            pre_[i] = preLevel_.next(preTarget);
    } else {
        preLevel_.reset(preTarget);
    }

    // The tap after them: what the track adds into the mix
    if (tapped && !tapPreFader_) {
        std::copy_n(bus_[0].data(), frames, tapLeft_);
        std::copy_n(bus_[1].data(), frames, tapRight_);
    }
}

void TrackProcessor::readKey(uint32_t delay, uint32_t frames, float *left, float *right) const
{
    float *out[kChannels] = {left, right};
    const uint32_t n = std::min(frames, rendered_);
    if (delay_[0].empty() || n == 0) {
        for (float *channel : out)
            std::fill_n(channel, frames, 0.0f);
        return;
    }
    constexpr uint32_t mask = kDelaySize - 1;
    delay = std::min(delay, kMaxCompensation);
    const uint32_t start = (blockStart_ - delay) & mask;
    const uint32_t first = std::min(n, kDelaySize - start);
    for (uint32_t c = 0; c < kChannels; ++c) {
        const float *line = delay_[c].data();
        std::copy_n(line + start, first, out[c]);
        std::copy_n(line, n - first, out[c] + first);
        std::fill(out[c] + n, out[c] + frames, 0.0f);
    }
}

void TrackProcessor::pull(const Feed &feed, const TransportState &transport, uint32_t frames)
{
    TrackProcessor &from = *feed.from;
    frames = std::min(frames, from.rendered_);
    // Another track's output: its sound after its gain and pan, as it would go to the master
    if (!feed.send) {
        for (uint32_t c = 0; c < kChannels; ++c) {
            const float *in = from.bus_[c].data();
            float *bus = bus_[c].data();
            for (uint32_t i = 0; i < frames; ++i)
                bus[i] += in[i];
        }
        return;
    }

    // A send: its level (or its lane's), gliding on this bus alone (each send feeds one bus)
    const RouteSend &send = *feed.send;
    if (send.slot >= kMaxSends || from.delay_[0].empty())
        return;
    float target = from.sendLevels_[send.slot].load(std::memory_order_relaxed);
    if (send.param != 0)
        from.chain.automateMixParam(transport, send.param, target);
    target = std::clamp(target, 0.0f, 1.0f);
    Smoothed &smooth = from.sendSmooth_[send.slot];
    // A send turned all the way down, or a muted track's pre-fader send, costs nothing once settled
    const bool preSilent = send.preFader && !from.preGliding_ && from.preLevel_.current() == 0.0f;
    // (A settled level is left unwritten: the smoothing lives in the source track, beside its other
    // sends', and buses on other cores writing it every block would fight over its cache lines.)
    if (target == 0.0f && smooth.current() < kSettled) {
        if (smooth.current() != 0.0f)
            smooth.reset(0.0f);
        return;
    }
    // The level a sample at a time while it glides, else one number for the block
    const float *gains = nullptr;
    const float level = target;
    if (std::fabs(smooth.current() - target) > kSettled) {
        float *g = scratch_.data();
        for (uint32_t i = 0; i < frames; ++i)
            g[i] = smooth.next(target);
        gains = g;
    } else if (smooth.current() != target) {
        smooth.reset(target);
    }
    if (preSilent)
        return;

    const uint32_t delay = std::min(send.delay, kMaxCompensation);
    // After the fader at the output's delay: the track's output, scaled
    if (!send.preFader && delay == from.outputDelay_) {
        for (uint32_t c = 0; c < kChannels; ++c) {
            const float *in = from.bus_[c].data();
            float *bus = bus_[c].data();
            if (gains)
                for (uint32_t i = 0; i < frames; ++i)
                    bus[i] += in[i] * gains[i];
            else
                for (uint32_t i = 0; i < frames; ++i)
                    bus[i] += in[i] * level;
        }
        return;
    }
    // Else its delay line at the send's own delay (in at most two runs around the ring), with the
    // fader's levels (post) or the mute's (pre: one number once it has settled)
    constexpr uint32_t mask = kDelaySize - 1;
    const uint32_t start = (from.blockStart_ - delay) & mask;
    const uint32_t first = std::min(frames, kDelaySize - start);
    const float *pre = send.preFader && from.preGliding_ ? from.pre_.data() : nullptr;
    const float preLevel = from.preLevel_.current();
    for (uint32_t c = 0; c < kChannels; ++c) {
        const float *line = from.delay_[c].data();
        const float *fader = send.preFader ? pre : from.ramp_[c].data();
        float *bus = bus_[c].data();
        for (uint32_t run = 0; run < 2; ++run) {
            const uint32_t begin = run == 0 ? 0 : first;
            const uint32_t end = run == 0 ? first : frames;
            const float *src = run == 0 ? line + start : line; // the run, from its first frame
            const float *in = src;
            float *out = bus + begin;
            const float *f = fader ? fader + begin : nullptr;
            const float *g = gains ? gains + begin : nullptr;
            const uint32_t n = end - begin;
            if (f && g)
                for (uint32_t i = 0; i < n; ++i)
                    out[i] += in[i] * f[i] * g[i];
            else if (f)
                for (uint32_t i = 0; i < n; ++i)
                    out[i] += in[i] * f[i] * level;
            else if (g)
                for (uint32_t i = 0; i < n; ++i)
                    out[i] += in[i] * preLevel * g[i];
            else
                for (uint32_t i = 0; i < n; ++i)
                    out[i] += in[i] * (preLevel * level);
        }
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
