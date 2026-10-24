#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace audio {

// Where a sound's hits begin (its transients) and how fast they come: for warping a clip
// (docs/design/warping-plan.md). For a background thread: it reads the whole sound.
struct OnsetAnalysis {
    std::vector<double> transients; // seconds, rising, at least 40 ms apart
    std::vector<float> novelty;     // the spectral flux, one value a hop
    double hopSeconds = 0.0;
};

// The transients of interleaved audio: log spectral flux over 1024-point frames at a quarter hop
// (2048 above 64 kHz), peaks over an adaptive threshold (the local median, plus a share of the
// mean that `sensitivity`, 0-1, lowers), at least 40 ms apart, each moved to where the
// envelope's rise begins. Silence (under -60 dBFS) has none.
OnsetAnalysis analyseOnsets(const float *interleaved, uint64_t frames, uint32_t channels, double sampleRate,
                            float sensitivity = 0.5f);

// The tempo the novelty pulses at, 80-160 BPM (an autocorrelation between 60 and 200, weighted
// toward 120, folded into that octave), or 0 when nothing repeats
double estimateTempo(const OnsetAnalysis &onsets);

// A loop is usually cut to whole bars: the tempo that makes `seconds` 1, 2, 4, 8 or 16 bars of
// 4/4 and is within 3% of `estimate` (or 2% of twice or half it), else 0
double tempoFromLength(double seconds, double estimate);

// A tempo written in a file's name: "Loop 120bpm", "Groove 87.5 BPM", "kit_96_a" (a whole number
// 60-200 alone between separators, which could be anything else too). 0 for none. `named` says
// whether "bpm" was written beside it.
double bpmFromName(const std::string &name, bool *named = nullptr);

}
