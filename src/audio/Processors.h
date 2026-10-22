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

// Clicks on every unit of the time signature while the transport plays (quarters in 4/4, eighths
// in 6/8), higher on the first of each bar. Without a tempo map, on every beat in 4/4.
class Metronome : public AudioProcessor {
public:
    Metronome();

    AudioParam gain{0.5f, 0.0f, 1.0f};
    std::atomic<bool> enabled{false};

    void prepare(double sampleRate, uint32_t maxFrames) override;
    void process(AudioBlock &block) override;

    // Clicks started so far (any thread), for tests and displays
    uint64_t clicks() const { return clicks_.load(std::memory_order_relaxed); }

    // A count-in: `clicks` clicks `unit` beats apart while the transport is stopped, at the tempo at
    // the playhead, the first of every `perBar` higher. Any thread. countInDone() turns true once
    // the last has passed.
    void startCountIn(int clicks, int perBar, double unit);
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
    std::atomic<int> countInPerBar_{4};
    std::atomic<double> countInUnit_{1.0};
    int countPerBar_ = 4;
    std::atomic<bool> countInCancel_{false};
    std::atomic<bool> countInDone_{false};
    bool counting_ = false;
    uint64_t countFrame_ = 0;
    uint64_t countEnd_ = 0;
    double countBeatFrames_ = 0.0;
    int64_t countLastBeat_ = -1;
};

}
