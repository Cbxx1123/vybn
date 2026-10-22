#include "Processors.h"

#include <cmath>

namespace audio {

namespace {

constexpr double kTwoPi = 6.283185307179586;

}

// ToneGenerator

ToneGenerator::ToneGenerator()
    : AudioProcessor("Test tone")
{
}

void ToneGenerator::prepare(double sampleRate, uint32_t)
{
    sampleRate_ = sampleRate;
    level_.prepare(sampleRate, 15.0);
    level_.reset(0.0f);
    pitch_.prepare(sampleRate, 30.0);
    pitch_.reset(frequency.get());
    phase_ = 0.0;
}

void ToneGenerator::process(AudioBlock &block)
{
    const float targetLevel = enabled.load(std::memory_order_relaxed) ? gain.get() : 0.0f;
    // Silent and staying that way: nothing to add
    if (targetLevel == 0.0f && level_.current() < 1e-5f) {
        level_.reset(0.0f);
        return;
    }
    const float targetPitch = frequency.get();
    for (uint32_t i = 0; i < block.frames; ++i) {
        const float level = level_.next(targetLevel);
        phase_ += pitch_.next(targetPitch) / sampleRate_;
        phase_ -= std::floor(phase_);
        const float sample = level * static_cast<float>(std::sin(kTwoPi * phase_));
        for (uint32_t c = 0; c < block.channelCount; ++c)
            block.channels[c][i] += sample;
    }
}

// Metronome

Metronome::Metronome()
    : AudioProcessor("Metronome")
{
}

void Metronome::prepare(double sampleRate, uint32_t)
{
    sampleRate_ = sampleRate;
    clickLength_ = static_cast<uint32_t>(sampleRate * 0.035); // 35 ms
    clickRemaining_ = 0;
    lastBeat_ = -1;
}

void Metronome::startCountIn(int clicks, int perBar, double unit)
{
    countInDone_.store(false, std::memory_order_release);
    countInCancel_.store(false, std::memory_order_release);
    countInPerBar_.store(std::max(1, perBar), std::memory_order_relaxed);
    countInUnit_.store(unit > 0.0 ? unit : 1.0, std::memory_order_relaxed);
    countInRequest_.store(clicks, std::memory_order_release);
}

namespace {

// The click unit a beat is in: an id (bar and unit), whether it starts a bar, and where it ends
struct ClickUnit {
    int64_t id;
    bool downbeat;
    double end;
};

ClickUnit clickUnitAt(const TransportState &t, double beat)
{
    if (!t.map) {
        const double b = std::floor(beat);
        return {static_cast<int64_t>(b), static_cast<int64_t>(b) % 4 == 0, b + 1.0};
    }
    const int bar = t.map->barAt(beat);
    const double start = t.map->barStart(bar);
    const Signature &s = t.map->signatureAtBar(bar);
    const int unit = std::clamp(static_cast<int>(std::floor((beat - start) / s.unit() + 1e-9)), 0, s.numerator - 1);
    return {static_cast<int64_t>(bar) * 64 + unit, unit == 0, start + (unit + 1) * s.unit()};
}

}

void Metronome::cancelCountIn()
{
    countInRequest_.store(0, std::memory_order_release);
    countInCancel_.store(true, std::memory_order_release);
}

void Metronome::process(AudioBlock &block)
{
    const TransportState &t = block.transport;
    if (!t.playing) {
        // Count again from the next beat when playback resumes
        lastBeat_ = -1;
    }
    const bool on = enabled.load(std::memory_order_relaxed);
    const float g = gain.get();

    // Starting a count-in takes the tempo now; the count runs for its clicks, then reports done
    if (const int clicks = countInRequest_.exchange(0, std::memory_order_acq_rel); clicks > 0) {
        counting_ = true;
        countFrame_ = 0;
        countLastBeat_ = -1;
        countPerBar_ = countInPerBar_.load(std::memory_order_relaxed);
        countBeatFrames_ = sampleRate_ * 60.0 / t.tempo * countInUnit_.load(std::memory_order_relaxed);
        countEnd_ = static_cast<uint64_t>(clicks * countBeatFrames_ + 0.5);
    }
    // The unit the block starts in, and the frame the next one starts on
    ClickUnit unit{};
    double nextFrame = 0.0;
    if (t.playing) {
        unit = clickUnitAt(t, t.beatAt(0));
        nextFrame = t.frameAt(unit.end);
    }
    if (countInCancel_.exchange(false, std::memory_order_acq_rel))
        counting_ = false;

    for (uint32_t i = 0; i < block.frames; ++i) {
        if (counting_) {
            if (countFrame_ >= countEnd_) {
                counting_ = false;
                countInDone_.store(true, std::memory_order_release);
            } else {
                const int64_t beat = static_cast<int64_t>(std::floor(static_cast<double>(countFrame_) / countBeatFrames_));
                if (beat != countLastBeat_) {
                    countLastBeat_ = beat;
                    clickFrequency_ = beat % countPerBar_ == 0 ? 1760.0 : 1175.0;
                    clickPhase_ = 0.0;
                    clickRemaining_ = clickLength_;
                    clicks_.fetch_add(1, std::memory_order_relaxed);
                }
                ++countFrame_;
            }
        }
        if (t.playing) {
            if (i >= nextFrame) {
                unit = clickUnitAt(t, t.beatAt(i));
                nextFrame = t.frameAt(unit.end);
            }
            if (unit.id != lastBeat_) {
                lastBeat_ = unit.id;
                if (on) {
                    // The first beat of the bar is a fifth higher
                    clickFrequency_ = unit.downbeat ? 1760.0 : 1175.0;
                    clickPhase_ = 0.0;
                    clickRemaining_ = clickLength_;
                    clicks_.fetch_add(1, std::memory_order_relaxed);
                }
            }
        }
        if (clickRemaining_ == 0)
            continue;
        // A short sine burst with a fast exponential decay
        const double age = static_cast<double>(clickLength_ - clickRemaining_) / clickLength_;
        const float envelope = static_cast<float>(std::exp(-6.0 * age));
        const float sample = g * envelope * static_cast<float>(std::sin(kTwoPi * clickPhase_));
        clickPhase_ += clickFrequency_ / sampleRate_;
        --clickRemaining_;
        for (uint32_t c = 0; c < block.channelCount; ++c)
            block.channels[c][i] += sample;
    }
}

}
