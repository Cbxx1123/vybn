#include "ClipPlayer.h"

#include <algorithm>
#include <cmath>

namespace audio {

namespace {

constexpr double kHalfPi = 1.5707963267948966;
// How far ahead of the playhead streamed clips ask for their audio: well past a disk read, so a
// clip starting mid-file (trimmed, or the playhead moved into it) has its chunk when it begins
constexpr double kLookaheadSeconds = 0.5;

// Equal-power fade: 0 to 1 as t goes 0 to 1
float fadeCurve(double t)
{
    return t >= 1.0 ? 1.0f : t <= 0.0 ? 0.0f : static_cast<float>(std::sin(t * kHalfPi));
}

}

void ClipSequence::sort()
{
    std::stable_sort(regions.begin(), regions.end(),
                     [](const ClipRegion &a, const ClipRegion &b) { return a.start < b.start; });
}

void ClipSequence::crossfade(const TempoMap &timing)
{
    for (size_t i = 0; i < regions.size(); ++i) {
        for (size_t j = i + 1; j < regions.size(); ++j) {
            ClipRegion &a = regions[i];
            ClipRegion &b = regions[j];
            if (b.start >= a.end)
                continue;
            // b starts inside a: the overlap, but not past either's end
            const double overlap = timing.secondsAt(std::min(a.end, b.end)) - timing.secondsAt(b.start);
            if (b.end <= a.end)
                continue; // b sits wholly inside a; leave both alone
            a.fadeOut = std::max(a.fadeOut, overlap);
            b.fadeIn = std::max(b.fadeIn, overlap);
        }
    }
}

AudioClipPlayer::AudioClipPlayer() : AudioProcessor("Clips") {}

AudioClipPlayer::~AudioClipPlayer()
{
    delete current_;
    ClipSequence *s = nullptr;
    while (incoming_.pop(s))
        delete s;
    while (retired_.pop(s))
        delete s;
}

bool AudioClipPlayer::setSequence(std::unique_ptr<ClipSequence> sequence)
{
    if (incoming_.full())
        return false;
    incoming_.push(sequence.release());
    return true;
}

void AudioClipPlayer::collect()
{
    ClipSequence *s = nullptr;
    while (retired_.pop(s))
        delete s;
}

void AudioClipPlayer::process(AudioBlock &block)
{
    // The newest sequence, if retiring the old one has room
    ClipSequence *next = nullptr;
    while (!retired_.full() && incoming_.pop(next)) {
        if (current_)
            retired_.push(current_);
        current_ = next;
    }

    const uint32_t outputs = std::min<uint32_t>(block.channelCount, 2);
    const TransportState &t = block.transport;
    if (current_ && outputs > 0) {
        // Streamed clips under the playhead or starting soon ask for their audio ahead of time,
        // playing or not (so pressing play finds it in memory)
        const double nowSeconds = static_cast<double>(t.position) / t.sampleRate;
        const double now = t.beatAt(0);
        const double ahead = t.map ? t.map->beatAt(nowSeconds + kLookaheadSeconds) : now + kLookaheadSeconds * t.tempo / 60.0;
        for (const ClipRegion &r : current_->regions) {
            if (r.start >= ahead)
                break;
            if (r.end <= now || !r.data || !r.data->streamed())
                continue;
            const double elapsed = std::max(0.0, nowSeconds - t.secondsAt(r.start));
            r.data->prefetch(static_cast<uint64_t>(std::max(0.0, (r.offset + elapsed) * r.data->sampleRate)));
        }
    }
    if (current_ && t.playing && outputs > 0) {
        const double first = t.beatAt(0);
        const double last = t.beatAt(block.frames);
        const double firstSeconds = static_cast<double>(t.position) / t.sampleRate;
        for (const ClipRegion &r : current_->regions) {
            if (r.start >= last)
                break; // sorted: nothing later starts in this block
            if (r.end <= first || !r.data || r.data->frames == 0)
                continue;
            const AudioData &d = *r.data;
            // Time in the clip goes with the samples, whatever the tempo does
            const double startSeconds = t.secondsAt(r.start);
            const auto from = static_cast<uint32_t>(std::clamp(t.frameAt(r.start), 0.0, double(block.frames)));
            const auto to = static_cast<uint32_t>(std::clamp(std::ceil(t.frameAt(r.end)), 0.0, double(block.frames)));
            const double length = t.secondsAt(r.end) - startSeconds;
            const double rate = d.sampleRate;
            const uint32_t ch = d.channels;
            AudioData::Span span; // fresh each block: a streamed chunk's pointer lasts one callback
            for (uint32_t i = from; i < to; ++i) {
                const double elapsed = firstSeconds + i / t.sampleRate - startSeconds;
                if (elapsed < 0.0 || elapsed >= length)
                    continue;
                const double pos = (r.offset + elapsed) * rate;
                if (pos < 0.0)
                    continue;
                const auto index = static_cast<uint64_t>(pos);
                if (index + 1 >= d.frames)
                    continue;
                const float frac = static_cast<float>(pos - static_cast<double>(index));
                float g = r.gain;
                if (r.fadeIn > 0.0)
                    g *= fadeCurve(elapsed / r.fadeIn);
                if (r.fadeOut > 0.0)
                    g *= fadeCurve((length - elapsed) / r.fadeOut);
                // The two frames either side, possibly in neighbouring chunks; a chunk not yet in
                // memory (asked for now) leaves this clip silent for the rest of the block
                const float *a = d.frameAt(index, span);
                const float *b = a ? d.frameAt(index + 1, span) : nullptr;
                if (!a || !b)
                    break;
                const float left = a[0] + (b[0] - a[0]) * frac;
                const float right = ch > 1 ? a[1] + (b[1] - a[1]) * frac : left;
                block.channels[0][i] += left * g;
                if (outputs > 1)
                    block.channels[1][i] += right * g;
            }
        }
    }

    // Input monitoring, playing or not
    if (monitor.load(std::memory_order_relaxed) && block.inputs) {
        const int first = inputChannel.load(std::memory_order_relaxed);
        if (first >= 0 && static_cast<uint32_t>(first) < block.inputCount) {
            const float *l = block.inputs[first];
            const bool stereo = inputStereo.load(std::memory_order_relaxed) &&
                                static_cast<uint32_t>(first + 1) < block.inputCount;
            const float *r = stereo ? block.inputs[first + 1] : l;
            for (uint32_t i = 0; i < block.frames; ++i) {
                block.channels[0][i] += l[i];
                if (outputs > 1)
                    block.channels[1][i] += r[i];
            }
        }
    }
}

}
