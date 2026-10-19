#pragma once

#include <atomic>
#include <cstdint>

#include "AudioProcessor.h"

namespace audio {

// A sine wave, for testing outputs and levels. Off until enabled; fades in and out without
// clicks.
class ToneGenerator : public AudioProcessor {
public:
    ToneGenerator();

    AudioParam frequency{440.0f, 20.0f, 20000.0f}; // Hz
    AudioParam gain{0.2f, 0.0f, 1.0f};             // linear
    std::atomic<bool> enabled{false};

    void prepare(double sampleRate, uint32_t maxFrames) override;
    void process(AudioBlock &block) override;

private:
    double phase_ = 0.0; // 0-1
    double sampleRate_ = 48000.0;
    Smoothed level_;
    Smoothed pitch_;
};

// Clicks on every beat while the transport plays, higher on the first beat of each bar.
class Metronome : public AudioProcessor {
public:
    Metronome();

    AudioParam gain{0.5f, 0.0f, 1.0f};
    std::atomic<bool> enabled{false};
    std::atomic<int> beatsPerBar{4};

    void prepare(double sampleRate, uint32_t maxFrames) override;
    void process(AudioBlock &block) override;

    // Clicks started so far (any thread), for tests and displays
    uint64_t clicks() const { return clicks_.load(std::memory_order_relaxed); }

    // A count-in: clicks for `beats` beats while the transport is stopped, at the transport's tempo.
    // Any thread. countInDone() turns true once the last beat has passed.
    void startCountIn(int beats);
    void cancelCountIn();
    bool countInDone() const { return countInDone_.load(std::memory_order_acquire); }

private:
    double sampleRate_ = 48000.0;
    int64_t lastBeat_ = -1;
    double clickPhase_ = 0.0;
    double clickFrequency_ = 0.0;
    uint32_t clickRemaining_ = 0;
    uint32_t clickLength_ = 0;
    std::atomic<uint64_t> clicks_{0};

    // Count-in: the main thread sets the requests; the audio thread runs the count
    std::atomic<int> countInRequest_{0};
    std::atomic<bool> countInCancel_{false};
    std::atomic<bool> countInDone_{false};
    bool counting_ = false;
    uint64_t countFrame_ = 0;
    uint64_t countEnd_ = 0;
    double countBeatFrames_ = 0.0;
    int64_t countLastBeat_ = -1;
};

}
