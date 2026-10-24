#pragma once

#include <cstddef>
#include <vector>

namespace audio {

// The FFT of a real signal of `size` samples (a power of two, at least 4), through a complex FFT of
// half the size. Its tables are made in the constructor, so forward and inverse don't allocate.
// Any thread, one caller at a time per object (it keeps scratch buffers).
class RealFft {
public:
    explicit RealFft(size_t size);

    size_t size() const { return size_; }
    size_t bins() const { return size_ / 2 + 1; }

    // `in` holds size() samples; `re` and `im` get bins() values (the DC bin first, Nyquist last).
    // Unscaled: a full-scale sine of a bin's frequency gives a magnitude of size() / 2 there.
    void forward(const float *in, float *re, float *im);
    // The other way: bins() values to size() samples, so inverse(forward(x)) is x
    void inverse(const float *re, const float *im, float *out);

private:
    // The complex FFT of half the size, in place (sign -1 forward, +1 inverse, unscaled)
    void complexFft(float *re, float *im, int sign) const;

    size_t size_;
    size_t half_;
    std::vector<size_t> reverse_;     // bit reversal of half_
    std::vector<float> cos_, sin_;    // half_'s twiddles, e^-2pi i k / half
    std::vector<float> rotCos_, rotSin_; // the split's, e^-2pi i k / size, k <= half
    std::vector<float> zr_, zi_;      // scratch
};

// A periodic Hann window of `size` samples (sums to size / 2; overlapped at a quarter or half hop
// it sums to a constant)
std::vector<float> hannWindow(size_t size);

}
