#pragma once

#include <array>
#include <complex>
#include <cstddef>
#include <vector>

namespace audio::synth {

// The synth's wave bank: band-limited single-cycle waves, built once. A wave has `frames` cycles
// (one for the plain shapes, sixteen for the tables that morph), each of kTableSize samples and
// kMipLevels copies: level L holds only the harmonics up to (kTableSize / 2) >> L, so a note reads
// the level whose top harmonic stays below Nyquist at its pitch and nothing aliases. The
// harmonics are set and turned into samples by an inverse FFT.

// Choice indices of the Osc Wave parameters, in the order the parameter lists them
enum WaveId : int {
    kSine = 0,
    kTriangle,
    kSaw,
    kSquare,
    kBasicTable,
    kHarmonicTable,
    kVocalTable,
    kPwmTable,
    kWaveCount
};

constexpr int kTableSize = 2048;
constexpr int kMipLevels = 11;
constexpr int kTableFrames = 16;
// A level's samples: kTableSize of them and the first again, so interpolation needs no wrap
constexpr int kStride = kTableSize + 1;

// In-place radix-2 FFT of a power-of-two size. The inverse uses e^{+i} and isn't scaled.
void fft(std::vector<std::complex<double>> &a, bool inverse);

class WaveBank {
public:
    // Built on first use (a function-local static): the Synth constructor calls it on the main thread
    static const WaveBank &get();

    int frames(int wave) const { return waves_[static_cast<std::size_t>(wave)].frames; }
    // The kStride samples of one frame at one mip level
    const float *level(int wave, int frame, int mip) const;
    // The mip level for a phase increment (cycles per sample): the first whose top harmonic
    // times the increment stays below 0.5
    static int levelFor(double increment);

private:
    WaveBank();

    struct Wave {
        int frames = 1;
        // [frame][mip][kStride]
        std::vector<float> data;
    };
    std::array<Wave, kWaveCount> waves_;
};

}
