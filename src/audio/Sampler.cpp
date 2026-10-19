#include "Sampler.h"

#include "Memory.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace audio {

namespace {

constexpr float kSilent = 1e-4f;       // -80 dB: a releasing voice this quiet is done
constexpr float kChokeSeconds = 0.01f; // how fast a re-struck key cuts its previous note

}

Sampler::Sampler(std::string name)
    : Instrument(std::move(name))
{
}

Sampler::~Sampler() = default; // the audio thread has let go by now; holds_ frees the sets

bool Sampler::setSampleSet(std::shared_ptr<const SampleSet> set)
{
    if (!incoming_.push(set.get()))
        return false;
    if (set)
        holds_.push_back(set);
    latest_ = std::move(set);
    return true;
}

void Sampler::collect()
{
    const SampleSet *done = nullptr;
    while (retired_.pop(done)) {
        auto it = std::find_if(holds_.begin(), holds_.end(), [done](const auto &h) { return h.get() == done; });
        if (it != holds_.end())
            holds_.erase(it);
    }
}

void Sampler::prepare(double sampleRate, uint32_t)
{
    sampleRate_ = sampleRate;
    for (Voice &v : voices_)
        v.zone = nullptr;
}

void Sampler::release(Voice &voice, float seconds)
{
    voice.releasing = true;
    // Down 60 dB over `seconds`
    voice.decay = std::exp(std::log(0.001f) / (seconds * static_cast<float>(sampleRate_)));
}

Sampler::Voice &Sampler::freeVoice()
{
    Voice *best = nullptr;
    for (Voice &v : voices_) {
        if (!v.zone)
            return v;
        // Otherwise the quietest releasing voice, else the oldest
        if (!best || (v.releasing && (!best->releasing || v.envelope < best->envelope)) ||
            (!v.releasing && !best->releasing && v.started < best->started))
            best = &v;
    }
    return *best;
}

void Sampler::noteOn(int key, float velocity)
{
    if (!current_)
        return;
    // Every zone for the key at this velocity: layers (mic positions, a split stereo pair)
    // sound together
    const SampleZone *zones[kMaxLayers];
    const int midiVelocity = std::clamp(static_cast<int>(std::lround(velocity * 127.0f)), 1, 127);
    const int count = current_->zonesFor(key, midiVelocity, zones, kMaxLayers);
    if (count == 0)
        return;
    for (Voice &v : voices_) {
        if (v.zone && v.key == key && !v.releasing)
            release(v, kChokeSeconds);
    }
    for (int i = 0; i < count; ++i) {
        const SampleZone *zone = zones[i];
        if (zone->frames < 2)
            continue;
        Voice &v = freeVoice();
        v.zone = zone;
        v.key = key;
        v.position = 0.0;
        const float cents = (key - zone->rootKey) * zone->keytrackCents + zone->tuneCents;
        v.step = std::pow(2.0, cents / 1200.0) * zone->sampleRate / sampleRate_;
        v.level = std::clamp(velocity, 0.0f, 1.0f) * zone->gain;
        v.envelope = 1.0f;
        v.decay = 1.0f;
        v.releasing = false;
        v.started = ++notes_;
        // A streamed sample's first chunk is asked for now, while the preload plays
        if (zone->stream)
            zone->stream->prefetch(0);
    }
}

void Sampler::noteOff(int key)
{
    if (!current_)
        return;
    for (Voice &v : voices_) {
        if (v.zone && v.key == key && !v.releasing)
            release(v, current_->releaseSeconds);
    }
}

void Sampler::allNotesOff()
{
    for (Voice &v : voices_) {
        if (v.zone && !v.releasing)
            release(v, 0.05f);
    }
}

void Sampler::beginBlock()
{
    // A new sample set: the old one's voices stop at once, since its data is about to go
    const SampleSet *next = nullptr;
    bool changed = false;
    while (!retired_.full() && incoming_.pop(next)) {
        if (current_)
            retired_.push(current_);
        current_ = next;
        changed = true;
    }
    if (changed) {
        for (Voice &v : voices_)
            v.zone = nullptr;
    }
}

namespace {

// Where a voice reads from: a run of frames in memory, the preload or a streamed chunk. Only
// valid for the block it was found in (streamed chunks can go between blocks).
struct Segment {
    const int16_t *data = nullptr; // frame `begin`
    uint32_t begin = 0;
    uint32_t end = 0;
};

// The samples of `frame`, finding its segment when it isn't the current one; null when it is
// streamed and its chunk isn't in memory yet (it has been asked for)
inline const int16_t *frameAt(const SampleZone &z, uint32_t frame, Segment &seg)
{
    if (frame >= seg.begin && frame < seg.end)
        return seg.data + static_cast<size_t>(frame - seg.begin) * z.channels;
    const uint32_t resident = z.residentFrames();
    if (frame < resident) {
        seg = {z.pcm->data(), 0, resident};
    } else {
        const StreamSource *stream = z.stream.get();
        if (!stream || frame >= z.frames)
            return nullptr;
        const uint32_t k = stream->chunkOf(frame);
        const int16_t *chunk = stream->chunk(k);
        if (!chunk)
            return nullptr;
        stream->prefetch(k + 1); // the next one, a chunk ahead of the voice
        const uint32_t first = stream->preloadFrames + k * stream->chunkFrames;
        seg = {chunk, first, std::min(z.frames, first + stream->chunkFrames)};
    }
    return seg.data + static_cast<size_t>(frame - seg.begin) * z.channels;
}

}

void Sampler::controlChange(int controller, float value)
{
    value = std::clamp(value, 0.0f, 1.0f);
    switch (controller) {
    case 1: modWheel_ = value; break;
    case 7: volume_ = value; break;
    case 11: expression_ = value; break;
    case kPitchBend: bend_ = value; break;
    case 121: // reset all controllers
        modWheel_ = 0.0f;
        expression_ = 1.0f;
        bend_ = 0.5f;
        break;
    default: break;
    }
}

void Sampler::process(AudioBlock &block)
{
    beginBlock(); // a no-op when a NotePlayer already called it
    const float master = gain.get();
    // The controllers: a gain gliding from the last block's to this one's (volume's 100 is
    // unity, as in General MIDI), and one pitch factor for the block from the bend and the
    // vibrato (5.5 Hz, up to half a semitone)
    const float volume = volume_ / (100.0f / 127.0f);
    const float targetLevel = volume * volume * expression_ * expression_;
    const float startLevel = level_;
    const float levelStep = block.frames > 0 ? (targetLevel - startLevel) / static_cast<float>(block.frames) : 0.0f;
    level_ = targetLevel;
    vibrato_ += 5.5 * block.frames / sampleRate_;
    vibrato_ -= std::floor(vibrato_);
    const double semitones = (bend_ - 0.5) * 4.0 + modWheel_ * 0.5 * std::sin(vibrato_ * 6.283185307179586);
    const double pitch = semitones == 0.0 ? 1.0 : std::pow(2.0, semitones / 12.0);
    float *left = block.channels[0];
    float *right = block.channelCount > 1 ? block.channels[1] : nullptr;
    int active = 0;
    for (Voice &v : voices_) {
        if (!v.zone)
            continue;
        const SampleZone &z = *v.zone;
        if (!z.pcm)
            continue;
        Segment seg;
        const uint32_t last = z.frames - 1;
        const bool loops = z.loopEnd > z.loopStart;
        const double loopLength = static_cast<double>(z.loopEnd - z.loopStart);
        bool starved = false; // a streamed chunk wasn't in memory: silent until it is
        for (uint32_t i = 0; i < block.frames; ++i) {
            if (loops && v.position >= z.loopEnd)
                v.position -= loopLength * std::floor((v.position - z.loopStart) / loopLength);
            const uint32_t index = static_cast<uint32_t>(v.position);
            if (index >= last) {
                v.zone = nullptr;
                break;
            }
            const int16_t *a = starved ? nullptr : frameAt(z, index, seg);
            const int16_t *b = a ? frameAt(z, index + 1, seg) : nullptr;
            if (b) {
                const float frac = static_cast<float>(v.position - index);
                const float amp = v.level * v.envelope * master * (startLevel + levelStep * static_cast<float>(i)) * SampleZone::kScale;
                float l, r;
                if (z.channels == 2) {
                    l = a[0] + (b[0] - a[0]) * frac;
                    r = a[1] + (b[1] - a[1]) * frac;
                } else {
                    l = r = a[0] + (b[0] - a[0]) * frac;
                }
                left[i] += l * amp;
                if (right)
                    right[i] += r * amp;
            } else {
                starved = true; // keeps time, so the note is where it should be when the chunk comes
            }
            v.position += v.step * pitch;
            if (v.releasing) {
                v.envelope *= v.decay;
                if (v.envelope < kSilent) {
                    v.zone = nullptr;
                    break;
                }
            }
        }
        if (v.zone)
            ++active;
    }
    active_.store(active, std::memory_order_relaxed);
}

}
