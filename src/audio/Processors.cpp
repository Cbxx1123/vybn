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

void Metronome::startCountIn(int beats)
{
    countInDone_.store(false, std::memory_order_release);
    countInCancel_.store(false, std::memory_order_release);
    countInRequest_.store(beats, std::memory_order_release);
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
    const int bar = beatsPerBar.load(std::memory_order_relaxed);

    // Starting a count-in takes the tempo now; the count runs for its beats, then reports done
    if (const int beats = countInRequest_.exchange(0, std::memory_order_acq_rel); beats > 0) {
        counting_ = true;
        countFrame_ = 0;
        countLastBeat_ = -1;
        countBeatFrames_ = sampleRate_ * 60.0 / t.tempo;
        countEnd_ = static_cast<uint64_t>(beats * countBeatFrames_ + 0.5);
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
                    clickFrequency_ = (bar > 0 && beat % bar == 0) ? 1760.0 : 1175.0;
                    clickPhase_ = 0.0;
                    clickRemaining_ = clickLength_;
                    clicks_.fetch_add(1, std::memory_order_relaxed);
                }
                ++countFrame_;
            }
        }
        if (t.playing) {
            const int64_t beat = static_cast<int64_t>(std::floor(t.beatAt(i)));
            if (beat != lastBeat_) {
                lastBeat_ = beat;
                if (on) {
                    // The first beat of the bar is a fifth higher
                    clickFrequency_ = (bar > 0 && beat % bar == 0) ? 1760.0 : 1175.0;
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
