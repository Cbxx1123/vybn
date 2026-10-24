#include "audio/ClipPlayer.h"

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

AudioClipPlayer::AudioClipPlayer()
    : AudioProcessor("Clips"), scratchLeft_(kMaxStretchBlock), scratchRight_(kMaxStretchBlock)
{
}

AudioClipPlayer::~AudioClipPlayer()
{
    for (int i = 0; i < voiceCount_; ++i)
        delete voices_[i];
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
    // A voice for each stretched region, up to kVoicesPerMode of a mode: those it hasn't got yet go
    // with the sequence
    int needed[kModes] = {};
    for (const ClipRegion &r : sequence->regions) {
        if (r.data && r.warp.stretched())
            ++needed[static_cast<int>(r.warp.mode)];
    }
    for (int m = 0; m < kModes; ++m) {
        for (; granted_[m] < std::min(needed[m], kVoicesPerMode); ++granted_[m])
            sequence->voices.push_back(makeStretchVoice(static_cast<WarpMode>(m)));
    }
    incoming_.push(sequence.release());
    return true;
}

StretchVoice *AudioClipPlayer::bindVoice(const ClipRegion &region, uint64_t key)
{
    StretchVoice *free = nullptr;
    for (int i = 0; i < voiceCount_; ++i) {
        StretchVoice *v = voices_[i];
        if (v->mode() != region.warp.mode)
            continue;
        if (v->key == key) {
            v->used = true;
            return v;
        }
        if (!free && v->key == 0)
            free = v;
    }
    if (free) {
        free->unbind();
        free->key = key;
        free->used = true;
    }
    return free;
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
        // Its new voices join the player's (the pointers move; the vector is freed with the sequence)
        for (auto &v : next->voices) {
            if (v && voiceCount_ < kModes * kVoicesPerMode)
                voices_[voiceCount_++] = v.release();
        }
        if (current_)
            retired_.push(current_);
        current_ = next;
    }
    for (int i = 0; i < voiceCount_; ++i)
        voices_[i]->used = false;

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
            if (r.warp.on) {
                // Where its map reaches now (or at its start) and half a second on
                const WarpPath path{&t, &r};
                const double at = r.start > now ? r.contentStart : path.content(0.0);
                const double rate = r.data->sampleRate;
                r.data->prefetch(static_cast<uint64_t>(std::max(0.0, warpSeconds(r.warp, at) * rate)));
                r.data->prefetch(static_cast<uint64_t>(std::max(0.0, warpSeconds(r.warp, at + (ahead - now)) * rate)));
                continue;
            }
            const double elapsed = std::max(0.0, nowSeconds - t.secondsAt(r.start));
            r.data->prefetch(static_cast<uint64_t>(std::max(0.0, (r.offset + elapsed) * r.data->sampleRate)));
        }
    }
    if (current_ && t.playing && outputs > 0) {
        const double first = t.beatAt(0);
        const double last = t.beatAt(block.frames);
        const double firstSeconds = static_cast<double>(t.position) / t.sampleRate;
        for (size_t index = 0; index < current_->regions.size(); ++index) {
            const ClipRegion &r = current_->regions[index];
            if (r.start >= last)
                break; // sorted: nothing later starts in this block
            if (r.end <= first || !r.data || r.data->frames == 0)
                continue;
            const AudioData &d = *r.data;
            if (r.warp.on) {
                // Warped: through the map (a voice stretching it, or read at its speed), then its gain
                // and fades as below
                const double startSeconds = t.secondsAt(r.start);
                const auto from = static_cast<uint32_t>(std::clamp(std::ceil(t.frameAt(r.start)), 0.0, double(block.frames)));
                const auto to = static_cast<uint32_t>(std::clamp(std::ceil(t.frameAt(r.end)), 0.0, double(block.frames)));
                if (to <= from)
                    continue;
                const WarpPath path{&t, &r};
                if (r.warp.stretched()) {
                    StretchVoice *v = bindVoice(r, r.key ? r.key : (index + 1) | (1ull << 63));
                    if (!v)
                        continue; // more stretched clips at once than voices
                    v->render(r, path, t, from, to, scratchLeft_.data(), scratchRight_.data());
                } else {
                    renderRepitch(r, path, from, to, scratchLeft_.data(), scratchRight_.data());
                }
                const double length = t.secondsAt(r.end) - startSeconds;
                for (uint32_t i = from; i < to; ++i) {
                    const double elapsed = firstSeconds + i / t.sampleRate - startSeconds;
                    float g = r.gain;
                    if (r.fadeIn > 0.0)
                        g *= fadeCurve(elapsed / r.fadeIn);
                    if (r.fadeOut > 0.0)
                        g *= fadeCurve((length - elapsed) / r.fadeOut);
                    block.channels[0][i] += scratchLeft_[i] * g;
                    if (outputs > 1)
                        block.channels[1][i] += scratchRight_[i] * g;
                }
                continue;
            }
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

    // Voices whose region didn't play are free again
    for (int i = 0; i < voiceCount_; ++i) {
        if (voices_[i]->key != 0 && !voices_[i]->used)
            voices_[i]->unbind();
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
