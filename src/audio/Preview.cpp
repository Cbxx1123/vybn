#include "Preview.h"

#include <algorithm>

namespace audio {

namespace {

constexpr float kFadeSeconds = 0.008f;

// Adds one sound into the block from `cursor` (frames into it), linearly resampled, with the
// level running from `level` by `slope` per frame; returns false once it has ended
bool render(const AudioData &d, double &cursor, double step, float level, float slope, float gain, AudioBlock &block)
{
    float *left = block.channels[0];
    float *right = block.channelCount > 1 ? block.channels[1] : nullptr;
    const uint64_t last = d.frames > 0 ? d.frames - 1 : 0;
    AudioData::Span span; // fresh each block: a streamed chunk's pointer lasts one callback
    for (uint32_t i = 0; i < block.frames; ++i) {
        const uint64_t index = static_cast<uint64_t>(cursor);
        if (index >= last)
            return false;
        const float *a = d.frameAt(index, span);
        const float *b = a ? d.frameAt(index + 1, span) : nullptr;
        if (!a || !b) {
            // A streamed chunk not in memory yet (asked for): silent, keeping time
            cursor += step * static_cast<double>(block.frames - i);
            return true;
        }
        const float frac = static_cast<float>(cursor - static_cast<double>(index));
        float l, r;
        if (d.channels == 2) {
            l = a[0] + (b[0] - a[0]) * frac;
            r = a[1] + (b[1] - a[1]) * frac;
        } else {
            l = r = a[0] + (b[0] - a[0]) * frac;
        }
        const float amp = std::max(0.0f, level) * gain;
        left[i] += l * amp;
        if (right)
            right[i] += r * amp;
        level += slope;
        cursor += step;
    }
    return true;
}

}

SoundPreview::SoundPreview() : AudioProcessor("Preview") {}

bool SoundPreview::play(std::shared_ptr<const AudioData> data)
{
    if (!incoming_.push(data.get()))
        return false;
    if (data)
        holds_.push_back(std::move(data));
    return true;
}

void SoundPreview::collect()
{
    const AudioData *done = nullptr;
    while (retired_.pop(done)) {
        auto it = std::find_if(holds_.begin(), holds_.end(), [done](const auto &h) { return h.get() == done; });
        if (it != holds_.end())
            holds_.erase(it);
    }
}

void SoundPreview::prepare(double sampleRate, uint32_t) { sampleRate_ = sampleRate; }

void SoundPreview::process(AudioBlock &block)
{
    // A new sound (or none): the one playing fades out; one already fading is cut
    const AudioData *next = nullptr;
    while (!retired_.full() && incoming_.pop(next)) {
        if (fading_)
            retired_.push(fading_);
        fading_ = current_;
        fadeCursor_ = cursor_;
        fade_ = 1.0f;
        current_ = next;
        cursor_ = 0.0;
    }
    const float g = gain.get();
    if (fading_) {
        const float slope = -1.0f / (kFadeSeconds * static_cast<float>(sampleRate_));
        render(*fading_, fadeCursor_, fading_->sampleRate / sampleRate_, fade_, slope, g, block);
        fade_ += slope * static_cast<float>(block.frames);
        if (fade_ <= 0.0f && retired_.push(fading_))
            fading_ = nullptr; // else handed back next block
    }
    if (current_) {
        if (!render(*current_, cursor_, current_->sampleRate / sampleRate_, 1.0f, 0.0f, g, block) &&
            retired_.push(current_))
            current_ = nullptr; // else handed back next block (it renders nothing past its end)
    }
    playing_.store(current_ != nullptr, std::memory_order_relaxed);
    position_.store(current_ ? cursor_ / current_->sampleRate : 0.0, std::memory_order_relaxed);
}

}
