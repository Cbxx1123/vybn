#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "audio/Fft.h"

namespace audio {

struct AudioData;

// The start of a hit or a note found in a sound
struct Transient {
    uint64_t frame = 0;
    float strength = 0.0f; // 0-1, against the file's strongest
};

// What a sound file holds, for warping (docs/design/warp-plan.md): its transients, its tempo, whether
// it is a loop and how percussive it is. Worked out once a file on a background thread.
struct AudioAnalysis {
    double sampleRate = 48000.0;
    uint64_t frames = 0;
    std::vector<Transient> transients; // in order
    double bpm = 0.0;        // the tempo found, 0 when none was
    float confidence = 0.0f; // how clearly, 0-1
    bool loop = false;       // its length is a whole number of bars at that tempo (then exactly it)
    int loopBars = 0;
    double downbeat = 0.0;   // seconds: where its first beat is
    float percussive = 0.0f; // 0-1: how much of its sound comes in hits

    double seconds() const { return sampleRate > 0.0 ? static_cast<double>(frames) / sampleRate : 0.0; }
};

// Finds the transients and tempo of a sound fed to it in pieces (any length, so a long file is never
// whole in memory): a short FFT every few milliseconds, the spectral flux of its log magnitudes, an
// adaptive threshold and peak picking; the tempo from the flux's autocorrelation.
class OnsetAnalyser {
public:
    explicit OnsetAnalyser(double sampleRate);

    void add(const float *interleaved, size_t frames, uint32_t channels);
    AudioAnalysis finish();

private:
    void frame(); // one FFT frame from the window

    double rate_;
    size_t hop_, size_;
    Fft fft_;
    std::vector<float> window_, hann_, buffer_, re_, im_, previous_;
    size_t filled_ = 0; // samples in buffer_ since the last frame
    uint64_t frames_ = 0;
    std::vector<float> flux_;   // one per hop
    std::vector<float> energy_; // the energy of each kEnergyFrames run, for placing onsets finely
    double energyRun_ = 0.0;
    uint32_t energyCount_ = 0;
};

constexpr uint32_t kEnergyFrames = 64;

// The analysis of interleaved samples
AudioAnalysis analyseSamples(const float *interleaved, uint64_t frames, uint32_t channels, double sampleRate);
// A file decoded a piece at a time (WAV, FLAC, MP3); null with the reason in error
std::shared_ptr<AudioAnalysis> analyseAudioFile(const std::string &path, std::string &error);
// A sound already loaded: its samples when it holds them, else its file decoded again (a streamed
// file, so the stream's memory isn't touched)
std::shared_ptr<AudioAnalysis> analyseAudio(const AudioData &data, std::string &error);

// The tempo that best explains onsets at these frames and strengths (the autocorrelation of their
// pulses), 60-200 BPM; 0 when there are too few. For tests and for guessing from a part of a file.
double tempoOfFlux(const std::vector<float> &flux, double hopsPerSecond, float *confidence = nullptr);

}
