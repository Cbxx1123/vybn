#include "audio/Fft.h"

#include <cmath>
#include <utility>

namespace audio {

namespace {

constexpr double kTwoPi = 6.283185307179586;

}

Fft::Fft(size_t size) : n_(size), half_(size / 2)
{
    size_t bits = 0;
    while ((size_t(1) << bits) < half_)
        ++bits;
    reversed_.resize(half_);
    for (size_t i = 0; i < half_; ++i) {
        size_t r = 0;
        for (size_t b = 0; b < bits; ++b)
            r |= ((i >> b) & 1u) << (bits - 1 - b);
        reversed_[i] = r;
    }
    cos_.resize(half_ / 2 + 1);
    sin_.resize(half_ / 2 + 1);
    for (size_t k = 0; k < cos_.size(); ++k) {
        cos_[k] = static_cast<float>(std::cos(kTwoPi * static_cast<double>(k) / static_cast<double>(half_)));
        sin_[k] = static_cast<float>(std::sin(kTwoPi * static_cast<double>(k) / static_cast<double>(half_)));
    }
    wcos_.resize(half_ + 1);
    wsin_.resize(half_ + 1);
    for (size_t k = 0; k <= half_; ++k) {
        wcos_[k] = static_cast<float>(std::cos(kTwoPi * static_cast<double>(k) / static_cast<double>(n_)));
        wsin_[k] = static_cast<float>(-std::sin(kTwoPi * static_cast<double>(k) / static_cast<double>(n_)));
    }
    zr_.resize(half_);
    zi_.resize(half_);
}

void Fft::transform(bool inverse)
{
    for (size_t i = 0; i < half_; ++i) {
        const size_t r = reversed_[i];
        if (r > i) {
            std::swap(zr_[i], zr_[r]);
            std::swap(zi_[i], zi_[r]);
        }
    }
    const float sign = inverse ? 1.0f : -1.0f;
    for (size_t len = 2; len <= half_; len <<= 1) {
        const size_t step = half_ / len;
        const size_t mid = len / 2;
        for (size_t i = 0; i < half_; i += len) {
            for (size_t j = 0; j < mid; ++j) {
                const float wr = cos_[j * step];
                const float wi = sign * sin_[j * step];
                const size_t a = i + j;
                const size_t b = a + mid;
                const float tr = zr_[b] * wr - zi_[b] * wi;
                const float ti = zr_[b] * wi + zi_[b] * wr;
                zr_[b] = zr_[a] - tr;
                zi_[b] = zi_[a] - ti;
                zr_[a] += tr;
                zi_[a] += ti;
            }
        }
    }
}

void Fft::forward(const float *in, float *re, float *im)
{
    // The even samples as the real part, the odd as the imaginary, through the half-size transform
    for (size_t i = 0; i < half_; ++i) {
        zr_[i] = in[2 * i];
        zi_[i] = in[2 * i + 1];
    }
    transform(false);
    // Split: X[k] = Fe[k] + W^k Fo[k], with Fe and Fo from Z[k] and conj(Z[half - k])
    for (size_t k = 0; k <= half_; ++k) {
        const size_t a = k == half_ ? 0 : k;
        const size_t b = k == 0 ? 0 : half_ - k;
        const float ar = zr_[a], ai = zi_[a];
        const float br = zr_[b], bi = -zi_[b];
        const float er = 0.5f * (ar + br), ei = 0.5f * (ai + bi);
        // (Z[k] - conj(Z[half - k])) / 2i
        const float dr = 0.5f * (ar - br), di = 0.5f * (ai - bi);
        const float orr = di, oi = -dr;
        const float wr = wcos_[k], wi = wsin_[k];
        re[k] = er + orr * wr - oi * wi;
        im[k] = ei + orr * wi + oi * wr;
    }
}

void Fft::inverse(const float *re, const float *im, float *out)
{
    for (size_t k = 0; k < half_; ++k) {
        const size_t b = half_ - k;
        const float ar = re[k], ai = im[k];
        const float br = re[b], bi = -im[b];
        const float er = 0.5f * (ar + br), ei = 0.5f * (ai + bi);
        const float dr = 0.5f * (ar - br), di = 0.5f * (ai - bi);
        // Fo = (X[k] - conj(X[half - k])) / 2 * e^(+2 pi i k / n)
        const float wr = wcos_[k], wi = -wsin_[k];
        const float orr = dr * wr - di * wi;
        const float oi = dr * wi + di * wr;
        // Z = Fe + i Fo
        zr_[k] = er - oi;
        zi_[k] = ei + orr;
    }
    transform(true);
    const float scale = 1.0f / static_cast<float>(half_);
    for (size_t i = 0; i < half_; ++i) {
        out[2 * i] = zr_[i] * scale;
        out[2 * i + 1] = zi_[i] * scale;
    }
}

}
