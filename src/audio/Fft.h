#pragma once

#include <cstddef>
#include <vector>

namespace audio {

// A real FFT of a power-of-two size, through a complex FFT of half the size. Its tables and work
// buffers are made in the constructor, so forward and inverse allocate nothing and can run on the
// audio thread (one caller at a time per object).
class Fft {
public:
    explicit Fft(size_t size);

    size_t size() const { return n_; }
    size_t bins() const { return n_ / 2 + 1; }

    // `in` holds size() samples; `re` and `im` get bins() values each
    void forward(const float *in, float *re, float *im);
    // The inverse of forward, scaled so that inverse(forward(x)) is x
    void inverse(const float *re, const float *im, float *out);

private:
    void transform(bool inverse); // the complex FFT of half the size on zr_, zi_, in place

    size_t n_ = 0;
    size_t half_ = 0;
    std::vector<size_t> reversed_;      // bit reversal of half_ indices
    std::vector<float> cos_, sin_;      // the half-size transform's twiddles (half_ / 2 of each)
    std::vector<float> wcos_, wsin_;    // e^(-2 pi i k / n) for the real split, half_ + 1 of each
    std::vector<float> zr_, zi_;
};

}
