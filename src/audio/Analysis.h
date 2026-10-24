#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "Fft.h"

namespace audio {

// Live analysis of what plays (docs/design/analyzer-plan.md).
//
// A tap: audio threads copy stereo frames in, the main thread drains them. Lock-free: a writer that
// finds another writing skips its block (counted), so a tap moved from one track to another never has
// two writers at once and nobody waits; a full ring drops what doesn't fit (counted). While `enabled`
// is off, writing costs one atomic load.
class AnalysisTap {
public:
    static constexpr uint32_t kFrames = 1u << 15;

    AnalysisTap();

    std::atomic<bool> enabled{false};

    // Audio threads
    void write(const float *left, const float *right, uint32_t frames);
    void writeInterleaved(const float *stereo, uint32_t frames);

    // Main thread: up to `frames` frames, interleaved left and right, oldest first; how many came
    size_t read(float *stereo, size_t frames);
    // Main thread: drops what is waiting
    void clear();
    uint64_t skipped() const { return skipped_.load(std::memory_order_relaxed); }
    uint64_t dropped() const { return dropped_.load(std::memory_order_relaxed); }

private:
    bool begin(uint32_t frames, uint64_t &head, uint32_t &fits);
    void end(uint64_t head, uint32_t written);

    std::unique_ptr<float[]> ring_; // kFrames stereo frames
    alignas(64) std::atomic<uint64_t> head_{0}; // frames written, ever
    alignas(64) std::atomic<uint64_t> tail_{0}; // frames read, ever
    std::atomic<bool> writing_{false};
    std::atomic<uint64_t> skipped_{0}, dropped_{0};
};

// The spectrum of the newest `size` frames (Hann window, left and right powers averaged) as levels on
// log-spaced bands from 20 Hz to 20 kHz, in dB where a full-scale sine reads 0. A band narrower than
// a bin reads the bins either side interpolated, a wider one its loudest bin. A tilt (dB per octave
// about 1 kHz) is added, the bands fall at `fallDbPerSecond` and peaks hold for `holdSeconds` (or
// until reset when it is negative) before falling. Main thread.
class SpectrumAnalyzer {
public:
    static constexpr size_t kBands = 256;
    static constexpr float kFloorDb = -120.0f;

    explicit SpectrumAnalyzer(size_t size = 8192);

    void setSize(size_t size); // a power of two, 1024-32768; starts again
    size_t size() const { return fft_.size(); }
    void setTilt(float dbPerOctave) { tilt_ = dbPerOctave; }
    float tilt() const { return tilt_; }
    void setFall(float dbPerSecond) { fall_ = dbPerSecond; }
    void setHold(float seconds) { hold_ = seconds; }
    float hold() const { return hold_; }

    void push(const float *stereo, size_t frames);
    // Analyses the newest frames for a frame `seconds` after the last (the falling and the holding)
    void update(double sampleRate, double seconds);
    void reset();

    const std::vector<float> &bands() const { return bands_; }
    const std::vector<float> &peaks() const { return peaks_; }
    static double bandFrequency(size_t band);
    // The level at a frequency (the band nearest it)
    float levelAt(double frequency) const;

private:
    RealFft fft_;
    std::vector<float> window_;
    float windowGain_ = 1.0f;
    std::vector<float> left_, right_; // the newest size() frames, a ring
    size_t write_ = 0;
    std::vector<float> frame_, re_, im_, power_;
    std::vector<float> bands_, peaks_, held_;
    float tilt_ = 0.0f, fall_ = 20.0f, hold_ = 3.0f;
};

// The stereo picture: the correlation over about 300 ms (+1 the same in both, 0 unrelated, -1 one
// the other turned over), the balance (-1 all left, +1 all right), the width (side over mid, in power)
// and the newest frames for a goniometer. Main thread.
class StereoMeter {
public:
    static constexpr size_t kPoints = 1024;

    void push(const float *stereo, size_t frames, double sampleRate);
    void reset();

    float correlation() const;
    float balance() const;
    float width() const;
    // The newest kPoints frames (left, right pairs), oldest first
    std::vector<float> points() const;

private:
    double lr_ = 0.0, ll_ = 0.0, rr_ = 0.0, mm_ = 0.0, ss_ = 0.0;
    std::vector<float> ring_ = std::vector<float>(kPoints * 2, 0.0f);
    size_t write_ = 0;
};

}
