#include "Fft.h"

#include <cmath>
#include <utility>

namespace audio {

namespace {

constexpr double kTwoPi = 6.283185307179586;

}

RealFft::RealFft(size_t size) : size_(size < 4 ? 4 : size), half_(size_ / 2)
{
    // Rounded up to a power of two
    size_t n = 4;
    while (n < size_)
        n *= 2;
    size_ = n;
    half_ = n / 2;

    int bits = 0;
    while ((size_t{1} << bits) < half_)
        ++bits;
    reverse_.resize(half_);
    for (size_t i = 0; i < half_; ++i) {
        size_t r = 0;
        for (int b = 0; b < bits; ++b)
            if (i & (size_t{1} << b))
                r |= size_t{1} << (bits - 1 - b);
        reverse_[i] = r;
    }
    cos_.resize(half_ / 2 + 1);
    sin_.resize(half_ / 2 + 1);
    for (size_t k = 0; k <= half_ / 2; ++k) {
        cos_[k] = static_cast<float>(std::cos(kTwoPi * static_cast<double>(k) / static_cast<double>(half_)));
        sin_[k] = static_cast<float>(-std::sin(kTwoPi * static_cast<double>(k) / static_cast<double>(half_)));
    }
    rotCos_.resize(half_ + 1);
    rotSin_.resize(half_ + 1);
    for (size_t k = 0; k <= half_; ++k) {
        rotCos_[k] = static_cast<float>(std::cos(kTwoPi * static_cast<double>(k) / static_cast<double>(size_)));
        rotSin_[k] = static_cast<float>(-std::sin(kTwoPi * static_cast<double>(k) / static_cast<double>(size_)));
    }
    zr_.resize(half_ + 1);
    zi_.resize(half_ + 1);
}

void RealFft::complexFft(float *re, float *im, int sign) const
{
    const size_t n = half_;
    for (size_t i = 0; i < n; ++i) {
        const size_t r = reverse_[i];
        if (r > i) {
            std::swap(re[i], re[r]);
            std::swap(im[i], im[r]);
        }
    }
    for (size_t length = 2; length <= n; length *= 2) {
        const size_t halfLength = length / 2;
        const size_t step = n / length; // into the twiddle table
        for (size_t start = 0; start < n; start += length) {
            for (size_t k = 0; k < halfLength; ++k) {
                // e^(sign 2pi i k / length): t is under half the table, which holds the first half-turn
                const size_t t = k * step;
                const float wr = cos_[t];
                const float wi = sign > 0 ? -sin_[t] : sin_[t];
                const size_t a = start + k;
                const size_t b = a + halfLength;
                const float xr = re[b] * wr - im[b] * wi;
                const float xi = re[b] * wi + im[b] * wr;
                re[b] = re[a] - xr;
                im[b] = im[a] - xi;
                re[a] += xr;
                im[a] += xi;
            }
        }
    }
}

void RealFft::forward(const float *in, float *re, float *im)
{
    // The even samples as the real part, the odd as the imaginary, through the half-size FFT
    const size_t m = half_;
    for (size_t k = 0; k < m; ++k) {
        zr_[k] = in[2 * k];
        zi_[k] = in[2 * k + 1];
    }
    complexFft(zr_.data(), zi_.data(), -1);
    zr_[m] = zr_[0];
    zi_[m] = zi_[0];
    // Split: X[k] = E[k] + e^(-2pi i k / n) O[k], where E and O come from Z[k] and conj(Z[m - k])
    for (size_t k = 0; k <= m; ++k) {
        const float ar = zr_[k], ai = zi_[k];
        const float br = zr_[m - k], bi = -zi_[m - k]; // conj(Z[m - k])
        const float er = 0.5f * (ar + br), ei = 0.5f * (ai + bi);
        // O = (Z[k] - conj(Z[m - k])) / 2i
        const float dr = ar - br, di = ai - bi;
        const float orr = 0.5f * di, oi = -0.5f * dr;
        const float wr = rotCos_[k], wi = rotSin_[k];
        re[k] = er + orr * wr - oi * wi;
        im[k] = ei + orr * wi + oi * wr;
    }
}

void RealFft::inverse(const float *re, const float *im, float *out)
{
    const size_t m = half_;
    for (size_t k = 0; k < m; ++k) {
        const float ar = re[k], ai = im[k];
        const float br = re[m - k], bi = -im[m - k]; // conj(X[m - k])
        const float er = 0.5f * (ar + br), ei = 0.5f * (ai + bi);
        // O = (X[k] - conj(X[m - k])) e^(2pi i k / n) / 2
        const float dr = 0.5f * (ar - br), di = 0.5f * (ai - bi);
        const float wr = rotCos_[k], wi = -rotSin_[k];
        const float orr = dr * wr - di * wi, oi = dr * wi + di * wr;
        // Z = E + i O
        zr_[k] = er - oi;
        zi_[k] = ei + orr;
    }
    complexFft(zr_.data(), zi_.data(), +1);
    const float scale = 1.0f / static_cast<float>(m);
    for (size_t k = 0; k < m; ++k) {
        out[2 * k] = zr_[k] * scale;
        out[2 * k + 1] = zi_[k] * scale;
    }
}

std::vector<float> hannWindow(size_t size)
{
    std::vector<float> w(size);
    for (size_t i = 0; i < size; ++i)
        w[i] = static_cast<float>(0.5 - 0.5 * std::cos(kTwoPi * static_cast<double>(i) / static_cast<double>(size)));
    return w;
}

}
