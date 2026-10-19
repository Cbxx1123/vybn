#include "SynthTables.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace audio::synth {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr int kHarmonics = kTableSize / 2; // the top harmonic of level 0

// Harmonic amplitudes of a sine series: a[n] multiplies sin(n * phase), n = 1..kHarmonics (a[0] unused)
using Spectrum = std::vector<double>;

Spectrum blank()
{
    return Spectrum(kHarmonics + 1, 0.0);
}

Spectrum sineSpectrum()
{
    Spectrum a = blank();
    a[1] = 1.0;
    return a;
}

// A rising saw from -1 to 1 over the cycle: -(2/pi) sum sin(n x) / n
Spectrum sawSpectrum()
{
    Spectrum a = blank();
    for (int n = 1; n <= kHarmonics; ++n)
        a[n] = -2.0 / (kPi * n);
    return a;
}

// A square wave: (4/pi) sum over odd n of sin(n x) / n
Spectrum squareSpectrum()
{
    Spectrum a = blank();
    for (int n = 1; n <= kHarmonics; n += 2)
        a[n] = 4.0 / (kPi * n);
    return a;
}

// A triangle: (8/pi^2) sum over odd n of (-1)^((n-1)/2) sin(n x) / n^2
Spectrum triangleSpectrum()
{
    Spectrum a = blank();
    for (int n = 1; n <= kHarmonics; n += 2) {
        const double sign = ((n - 1) / 2) % 2 == 0 ? 1.0 : -1.0;
        a[n] = 8.0 / (kPi * kPi) * sign / (static_cast<double>(n) * n);
    }
    return a;
}

// The Basic table's frame p (0-1 over the frames): the shapes morph sine, triangle, saw, square
Spectrum basicSpectrum(double p)
{
    const Spectrum shapes[4] = {sineSpectrum(), triangleSpectrum(), sawSpectrum(), squareSpectrum()};
    const double pos = std::min(p * 3.0, 3.0);
    const int i = std::min(static_cast<int>(pos), 2);
    const double t = pos - i;
    Spectrum a = blank();
    for (int n = 1; n <= kHarmonics; ++n)
        a[n] = (1.0 - t) * shapes[i][n] + t * shapes[i + 1][n];
    return a;
}

// The Harmonic table: harmonics 1..N at 1/sqrt(n), N growing with p
Spectrum harmonicSpectrum(double p)
{
    const int count = static_cast<int>(std::lround(1.0 + 63.0 * p * p));
    Spectrum a = blank();
    for (int n = 1; n <= std::min(count, kHarmonics); ++n)
        a[n] = 1.0 / std::sqrt(static_cast<double>(n));
    return a;
}

// The Vocal table: a saw at 220 Hz shaped by three formants that move through the vowels a e i o u
Spectrum vocalSpectrum(double p)
{
    static const double vowels[5][3] = {
        {800, 1150, 2900}, // a
        {400, 1600, 2700}, // e
        {350, 1700, 2700}, // i
        {450, 800, 2830}, // o
        {325, 700, 2530}, // u
    };
    static const double widths[3] = {80.0, 100.0, 150.0};
    static const double gains[3] = {1.0, 0.7, 0.4};
    const double pos = std::min(p * 4.0, 4.0);
    const int i = std::min(static_cast<int>(pos), 3);
    const double t = pos - i;
    double formant[3];
    for (int k = 0; k < 3; ++k)
        formant[k] = (1.0 - t) * vowels[i][k] + t * vowels[i + 1][k];
    Spectrum a = blank();
    for (int n = 1; n <= kHarmonics; ++n) {
        const double hz = n * 220.0;
        double weight = 0.0;
        for (int k = 0; k < 3; ++k) {
            const double d = (hz - formant[k]) / widths[k];
            weight += gains[k] * std::exp(-0.5 * d * d);
        }
        a[n] = -weight / n;
    }
    return a;
}

// The PWM table: a pulse whose width goes from half the cycle (p = 0) to 5% (p = 1)
Spectrum pwmSpectrum(double p)
{
    const double width = 0.5 - 0.45 * p;
    Spectrum a = blank();
    for (int n = 1; n <= kHarmonics; ++n)
        a[n] = 2.0 / (n * kPi) * std::sin(n * kPi * width);
    return a;
}

Spectrum spectrumOf(int wave, int frame)
{
    // Where the frame sits over the table, 0 to 1 (the plain shapes have one frame, at 0)
    const double p = static_cast<double>(frame) / (kTableFrames - 1);
    switch (wave) {
    case kSine: return sineSpectrum();
    case kTriangle: return triangleSpectrum();
    case kSaw: return sawSpectrum();
    case kSquare: return squareSpectrum();
    case kBasicTable: return basicSpectrum(p);
    case kHarmonicTable: return harmonicSpectrum(p);
    case kVocalTable: return vocalSpectrum(p);
    default: return pwmSpectrum(p);
    }
}

// Appends one frame's kMipLevels copies to `out`, normalised so the level-0 copy peaks at 1
void buildFrame(const Spectrum &a, std::vector<float> &out)
{
    const std::size_t base = out.size();
    out.resize(base + static_cast<std::size_t>(kMipLevels) * kStride);
    std::vector<std::complex<double>> z(kTableSize);
    double peak = 0.0;
    for (int mip = 0; mip < kMipLevels; ++mip) {
        const int top = kHarmonics >> mip;
        std::fill(z.begin(), z.end(), std::complex<double>(0.0, 0.0));
        // a sin(n x) is the real part of -i a e^{i n x}: positive harmonics only, real output
        for (int n = 1; n <= top; ++n)
            z[n] = std::complex<double>(0.0, -a[n]);
        fft(z, true);
        float *dst = &out[base + static_cast<std::size_t>(mip) * kStride];
        for (int i = 0; i < kTableSize; ++i) {
            dst[i] = static_cast<float>(z[i].real());
            if (mip == 0)
                peak = std::max(peak, std::fabs(static_cast<double>(dst[i])));
        }
        dst[kTableSize] = dst[0];
    }
    if (peak > 0.0) {
        const float scale = static_cast<float>(1.0 / peak);
        for (std::size_t i = base; i < out.size(); ++i)
            out[i] *= scale;
    }
}

}

void fft(std::vector<std::complex<double>> &a, bool inverse)
{
    const std::size_t n = a.size();
    for (std::size_t i = 1, j = 0; i < n; ++i) {
        std::size_t bit = n >> 1;
        for (; j & bit; bit >>= 1)
            j ^= bit;
        j ^= bit;
        if (i < j)
            std::swap(a[i], a[j]);
    }
    for (std::size_t len = 2; len <= n; len <<= 1) {
        const double angle = 2.0 * kPi / static_cast<double>(len) * (inverse ? 1.0 : -1.0);
        const std::complex<double> step(std::cos(angle), std::sin(angle));
        for (std::size_t i = 0; i < n; i += len) {
            std::complex<double> w(1.0, 0.0);
            for (std::size_t j = 0; j < len / 2; ++j) {
                const std::complex<double> u = a[i + j];
                const std::complex<double> v = a[i + j + len / 2] * w;
                a[i + j] = u + v;
                a[i + j + len / 2] = u - v;
                w *= step;
            }
        }
    }
}

const WaveBank &WaveBank::get()
{
    static const WaveBank bank;
    return bank;
}

WaveBank::WaveBank()
{
    for (int wave = 0; wave < kWaveCount; ++wave) {
        Wave &w = waves_[static_cast<std::size_t>(wave)];
        w.frames = wave >= kBasicTable ? kTableFrames : 1;
        w.data.reserve(static_cast<std::size_t>(w.frames) * kMipLevels * kStride);
        for (int frame = 0; frame < w.frames; ++frame)
            buildFrame(spectrumOf(wave, frame), w.data);
    }
}

const float *WaveBank::level(int wave, int frame, int mip) const
{
    const Wave &w = waves_[static_cast<std::size_t>(wave)];
    return &w.data[(static_cast<std::size_t>(frame) * kMipLevels + static_cast<std::size_t>(mip)) * kStride];
}

int WaveBank::levelFor(double increment)
{
    int mip = 0;
    while (mip < kMipLevels - 1 && static_cast<double>(kHarmonics >> mip) * increment >= 0.5)
        ++mip;
    return mip;
}

}
