#include "Stretch.h"

#include <algorithm>
#include <cmath>

#include "Fft.h"

namespace audio {

namespace {

constexpr double kPi = 3.141592653589793;
constexpr double kTwoPi = 6.283185307179586;

bool cancelled(const std::atomic<bool> *cancel)
{
    return cancel && cancel->load(std::memory_order_relaxed);
}

double principal(double phase)
{
    return phase - kTwoPi * std::floor((phase + kPi) / kTwoPi);
}

// Interleaved frames to read from, with silence outside them
struct Frames {
    const float *samples;
    int64_t frames;
    uint32_t channels;

    float at(int64_t frame, uint32_t channel) const
    {
        return frame >= 0 && frame < frames ? samples[frame * channels + channel] : 0.0f;
    }
    // Four-point (Catmull-Rom) interpolation at a fractional frame
    float cubic(double position, uint32_t channel) const
    {
        const double whole = std::floor(position);
        const auto i = static_cast<int64_t>(whole);
        const auto f = static_cast<float>(position - whole);
        const float y0 = at(i - 1, channel), y1 = at(i, channel), y2 = at(i + 1, channel), y3 = at(i + 2, channel);
        const float a = -0.5f * y0 + 1.5f * y1 - 1.5f * y2 + 0.5f * y3;
        const float b = y0 - 2.5f * y1 + 2.0f * y2 - 0.5f * y3;
        const float c = -0.5f * y0 + 0.5f * y2;
        return ((a * f + b) * f + c) * f + y1;
    }
};

Frames framesOf(const StretchSource &s)
{
    return {s.samples, static_cast<int64_t>(s.frames), s.channels};
}

// The output played along the path by interpolation: Repitch, and the last step of a transpose
std::vector<float> resampleAlong(const Frames &in, double rate, const TimePath &path, uint64_t outFrames,
                                 const std::atomic<bool> *cancel)
{
    std::vector<float> out(static_cast<size_t>(outFrames) * in.channels);
    for (uint64_t n = 0; n < outFrames; ++n) {
        if ((n & 65535) == 0 && cancelled(cancel))
            return {};
        const double position = path.at(static_cast<double>(n) / rate) * rate;
        for (uint32_t c = 0; c < in.channels; ++c)
            out[static_cast<size_t>(n) * in.channels + c] = in.cubic(position, c);
    }
    return out;
}

// A low-pass (a windowed sinc, `cutoff` a fraction of the rate) in place, before a resampling
// that reads faster than the audio was made, so what it folds down is gone first
void lowPass(std::vector<float> &audio, uint32_t channels, double cutoff)
{
    constexpr int kTaps = 33;
    float kernel[kTaps];
    float sum = 0.0f;
    for (int k = 0; k < kTaps; ++k) {
        const double x = k - (kTaps - 1) / 2.0;
        const double sinc = x == 0.0 ? 2.0 * cutoff : std::sin(kTwoPi * cutoff * x) / (kPi * x);
        const double blackman = 0.42 - 0.5 * std::cos(kTwoPi * k / (kTaps - 1)) + 0.08 * std::cos(2.0 * kTwoPi * k / (kTaps - 1));
        kernel[k] = static_cast<float>(sinc * blackman);
        sum += kernel[k];
    }
    for (float &k : kernel)
        k /= sum;
    const size_t frames = audio.size() / channels;
    std::vector<float> line(frames);
    for (uint32_t c = 0; c < channels; ++c) {
        for (size_t i = 0; i < frames; ++i)
            line[i] = audio[i * channels + c];
        for (size_t i = 0; i < frames; ++i) {
            float acc = 0.0f;
            for (int k = 0; k < kTaps; ++k) {
                const auto j = static_cast<int64_t>(i) + k - (kTaps - 1) / 2;
                if (j >= 0 && j < static_cast<int64_t>(frames))
                    acc += line[static_cast<size_t>(j)] * kernel[k];
            }
            audio[i * channels + c] = acc;
        }
    }
}

// The phase vocoder (Tonal), output `frames` long along `path` (output seconds to source seconds)
std::vector<float> tonal(const StretchSource &src, const TimePath &path, uint64_t frames, const std::vector<double> &transients,
                         const std::atomic<bool> *cancel)
{
    const Frames in = framesOf(src);
    const uint32_t ch = src.channels;
    const double rate = src.sampleRate;
    const size_t n = rate > 64000.0 ? 8192 : 4096;
    const size_t hop = n / 4;
    RealFft fft(n);
    const std::vector<float> window = hannWindow(n);
    const size_t bins = fft.bins();

    std::vector<float> out(static_cast<size_t>(frames) * ch, 0.0f);
    std::vector<float> norm(static_cast<size_t>(frames), 0.0f);
    std::vector<float> frame(n), refRe(bins), refIm(bins), prevRe(bins), prevIm(bins), re(bins), im(bins);
    std::vector<double> phaseA(bins), synth(bins, 0.0), next(bins);
    std::vector<float> magnitude(bins);
    std::vector<size_t> peaks;

    // A frame of the mono mix (or of one channel) starting `first`, windowed
    auto analyse = [&](int64_t first, int channel, float *outRe, float *outIm) {
        for (size_t k = 0; k < n; ++k) {
            const int64_t at = first + static_cast<int64_t>(k);
            float x = 0.0f;
            if (channel >= 0) {
                x = in.at(at, static_cast<uint32_t>(channel));
            } else {
                for (uint32_t c = 0; c < ch; ++c)
                    x += in.at(at, c);
                x /= static_cast<float>(ch);
            }
            frame[k] = x * window[k];
        }
        fft.forward(frame.data(), outRe, outIm);
    };

    double previousAt = -1e18;
    const size_t count = static_cast<size_t>(frames / hop) + 2;
    for (size_t j = 0; j < count; ++j) {
        if (cancelled(cancel))
            return {};
        const int64_t centre = static_cast<int64_t>(j * hop);
        const double at = path.at(static_cast<double>(centre) / rate) * rate; // the source frame playing here
        const auto first = static_cast<int64_t>(std::llround(at)) - static_cast<int64_t>(n / 2);
        analyse(first, ch == 1 ? 0 : -1, refRe.data(), refIm.data());
        for (size_t b = 0; b < bins; ++b) {
            phaseA[b] = std::atan2(refIm[b], refRe[b]);
            magnitude[b] = std::sqrt(refRe[b] * refRe[b] + refIm[b] * refIm[b]);
        }
        // A transient since the last frame resets the phases, so its attack is as recorded
        const auto t = std::upper_bound(transients.begin(), transients.end(), previousAt / rate);
        const bool reset = j == 0 || (t != transients.end() && *t * rate <= at);
        if (reset) {
            synth = phaseA;
        } else {
            // Each peak's phase moves by its frequency over the hop (measured from a frame a hop
            // before this one, so any analysis hop works); the bins around it keep their phase
            // relative to it (identity phase locking)
            analyse(first - static_cast<int64_t>(hop), ch == 1 ? 0 : -1, prevRe.data(), prevIm.data());
            peaks.clear();
            for (size_t b = 2; b + 2 < bins; ++b) {
                const float m = magnitude[b];
                if (m > magnitude[b - 1] && m >= magnitude[b + 1] && m > magnitude[b - 2] && m >= magnitude[b + 2])
                    peaks.push_back(b);
            }
            auto advance = [&](size_t b) {
                const double omega = kTwoPi * static_cast<double>(b) / static_cast<double>(n);
                const double before = std::atan2(prevIm[b], prevRe[b]);
                const double delta = principal(phaseA[b] - before - omega * static_cast<double>(hop)) + omega * static_cast<double>(hop);
                return principal(synth[b] + delta);
            };
            if (peaks.empty()) {
                for (size_t b = 0; b < bins; ++b)
                    next[b] = advance(b);
            } else {
                size_t start = 0;
                for (size_t p = 0; p < peaks.size(); ++p) {
                    const size_t peak = peaks[p];
                    // The region runs to the quietest bin before the next peak
                    size_t end = bins;
                    if (p + 1 < peaks.size()) {
                        end = peak;
                        for (size_t b = peak; b < peaks[p + 1]; ++b)
                            if (magnitude[b] < magnitude[end])
                                end = b;
                        end = std::max(end, peak + 1);
                    }
                    const double locked = advance(peak);
                    for (size_t b = start; b < end; ++b)
                        next[b] = principal(locked + phaseA[b] - phaseA[peak]);
                    start = end;
                }
            }
            std::swap(synth, next);
        }

        // Every channel turned by the reference's phase change, so the stereo image holds
        for (uint32_t c = 0; c < ch; ++c) {
            if (ch == 1) {
                re = refRe;
                im = refIm;
            } else {
                analyse(first, static_cast<int>(c), re.data(), im.data());
            }
            for (size_t b = 0; b < bins; ++b) {
                const double turn = synth[b] - phaseA[b];
                const float cr = static_cast<float>(std::cos(turn)), ci = static_cast<float>(std::sin(turn));
                const float r = re[b] * cr - im[b] * ci;
                const float i = re[b] * ci + im[b] * cr;
                re[b] = r;
                im[b] = i;
            }
            fft.inverse(re.data(), im.data(), frame.data());
            for (size_t k = 0; k < n; ++k) {
                const int64_t o = centre - static_cast<int64_t>(n / 2) + static_cast<int64_t>(k);
                if (o < 0 || o >= static_cast<int64_t>(frames))
                    continue;
                out[static_cast<size_t>(o) * ch + c] += frame[k] * window[k];
                if (c == 0)
                    norm[static_cast<size_t>(o)] += window[k] * window[k];
            }
        }
        previousAt = at;
    }
    for (size_t i = 0; i < static_cast<size_t>(frames); ++i) {
        const float g = 1.0f / std::max(norm[i], 1e-3f);
        for (uint32_t c = 0; c < ch; ++c)
            out[i * ch + c] *= g;
    }
    return out;
}

// WSOLA from the transients (Beats), output `frames` long along `path`
std::vector<float> beats(const StretchSource &src, const TimePath &path, uint64_t frames, const std::vector<double> &transients,
                         const std::atomic<bool> *cancel)
{
    const Frames in = framesOf(src);
    const uint32_t ch = src.channels;
    const double rate = src.sampleRate;
    const int64_t n = rate > 64000.0 ? 2048 : 1024;
    const int64_t hop = n / 2;
    const int64_t search = n / 4;
    const std::vector<float> window = hannWindow(static_cast<size_t>(n));
    const auto fadeIn = static_cast<int64_t>(0.001 * rate);
    const auto tail = static_cast<int64_t>(0.004 * rate);
    const auto total = static_cast<int64_t>(frames);

    std::vector<float> mono(static_cast<size_t>(src.frames));
    for (uint64_t i = 0; i < src.frames; ++i) {
        float sum = 0.0f;
        for (uint32_t c = 0; c < ch; ++c)
            sum += src.samples[i * ch + c];
        mono[static_cast<size_t>(i)] = sum;
    }
    auto monoAt = [&](int64_t i) { return i >= 0 && i < static_cast<int64_t>(mono.size()) ? mono[static_cast<size_t>(i)] : 0.0f; };
    // How alike the source is at `a` and `b` over the overlap (half a frame), every `step` samples
    auto similarity = [&](int64_t a, int64_t b, int64_t step) {
        double ab = 0.0, bb = 1e-9;
        for (int64_t k = 0; k < hop; k += step) {
            const double x = monoAt(a + k), y = monoAt(b + k);
            ab += x * y;
            bb += y * y;
        }
        return ab / std::sqrt(bb);
    };

    // The pieces: from the start, and from each transient inside the file
    std::vector<double> starts{0.0};
    const double length = static_cast<double>(src.frames) / rate;
    for (double t : transients)
        if (t > starts.back() + 0.005 && t < length)
            starts.push_back(t);

    std::vector<float> out(static_cast<size_t>(frames) * ch, 0.0f);
    std::vector<float> num, den;
    for (size_t s = 0; s < starts.size(); ++s) {
        if (cancelled(cancel))
            return {};
        const auto y0 = static_cast<int64_t>(std::llround(path.outputAt(starts[s]) * rate));
        const int64_t y1 = s + 1 < starts.size() ? static_cast<int64_t>(std::llround(path.outputAt(starts[s + 1]) * rate)) : total;
        if (y1 <= y0 || y0 >= total)
            continue;
        const int64_t segment = y1 - y0;
        const int64_t len = std::min(segment + tail, total - y0);
        if (len <= 0)
            continue;
        num.assign(static_cast<size_t>(len) * ch, 0.0f);
        den.assign(static_cast<size_t>(len), 0.0f);
        // The analysis stays in the piece (so a stretched piece never plays the next hit early)
        const auto pieceStart = static_cast<int64_t>(std::llround(starts[s] * rate));
        const int64_t pieceEnd = s + 1 < starts.size() ? static_cast<int64_t>(std::llround(starts[s + 1] * rate)) : static_cast<int64_t>(src.frames);
        const int64_t last = std::max(pieceStart, pieceEnd - n);
        int64_t previous = pieceStart;
        for (int64_t p = 0; p < len; p += hop) {
            int64_t a = pieceStart;
            if (p > 0) {
                const int64_t natural = previous + hop;
                const auto nominal = static_cast<int64_t>(std::llround(path.at(static_cast<double>(y0 + p) / rate) * rate));
                const int64_t low = std::clamp(nominal - search, pieceStart, last);
                const int64_t high = std::clamp(nominal + search, pieceStart, last);
                // Coarse then fine: the candidate most like the natural continuation
                int64_t best = std::clamp(natural, low, high);
                double bestScore = -1e30;
                for (int64_t c = low; c <= high; c += 4) {
                    const double score = similarity(natural, c, 4);
                    if (score > bestScore) {
                        bestScore = score;
                        best = c;
                    }
                }
                const int64_t coarse = best;
                bestScore = -1e30;
                for (int64_t c = std::max(low, coarse - 3); c <= std::min(high, coarse + 3); ++c) {
                    const double score = similarity(natural, c, 2);
                    if (score > bestScore) {
                        bestScore = score;
                        best = c;
                    }
                }
                a = best;
            }
            for (int64_t k = 0; k < n && p + k < len; ++k) {
                const float w = window[static_cast<size_t>(k)];
                for (uint32_t c = 0; c < ch; ++c)
                    num[static_cast<size_t>(p + k) * ch + c] += in.at(a + k, c) * w;
                den[static_cast<size_t>(p + k)] += w;
            }
            previous = a;
        }
        // Normalised by the window sum (the attack comes out as recorded), faded in over a
        // millisecond after the first piece and out over the tail past its end
        for (int64_t t = 0; t < len; ++t) {
            float g = 1.0f;
            if (s > 0 && t < fadeIn)
                g *= static_cast<float>(t) / static_cast<float>(fadeIn);
            if (t >= segment)
                g *= static_cast<float>(std::cos(0.5 * kPi * static_cast<double>(t - segment) / static_cast<double>(std::max<int64_t>(1, tail))));
            const float d = den[static_cast<size_t>(t)];
            if (d < 1e-4f)
                continue;
            g /= d;
            for (uint32_t c = 0; c < ch; ++c)
                out[static_cast<size_t>(y0 + t) * ch + c] += num[static_cast<size_t>(t) * ch + c] * g;
        }
    }
    return out;
}

}

double TimePath::at(double seconds) const
{
    if (source.empty())
        return seconds;
    if (source.size() == 1)
        return source.front() + seconds;
    const double x = seconds / step;
    const auto last = static_cast<double>(source.size() - 1);
    if (x <= 0.0)
        return source[0] + x * (source[1] - source[0]);
    if (x >= last)
        return source.back() + (x - last) * (source.back() - source[source.size() - 2]);
    const auto i = static_cast<size_t>(x);
    const double f = x - static_cast<double>(i);
    return source[i] + (source[i + 1] - source[i]) * f;
}

double TimePath::outputAt(double seconds) const
{
    if (source.empty())
        return seconds;
    if (source.size() == 1)
        return seconds - source.front();
    auto inverse = [](double y, double y0, double y1, double x0, double step) {
        const double dy = y1 - y0;
        return dy > 1e-12 ? x0 + (y - y0) / dy * step : x0;
    };
    if (seconds <= source.front())
        return inverse(seconds, source[0], source[1], 0.0, step);
    if (seconds >= source.back())
        return inverse(seconds, source[source.size() - 2], source.back(), static_cast<double>(source.size() - 2) * step, step);
    const auto it = std::upper_bound(source.begin(), source.end(), seconds);
    const auto i = static_cast<size_t>(it - source.begin()) - 1;
    return inverse(seconds, source[i], source[i + 1], static_cast<double>(i) * step, step);
}

std::vector<float> stretchAudio(const StretchSource &source, const TimePath &path, WarpMode mode, double pitch,
                                const std::vector<double> &transients, const std::atomic<bool> *cancel)
{
    if (!source.samples || source.frames == 0 || source.channels == 0 || source.sampleRate <= 0.0)
        return {};
    const double rate = source.sampleRate;
    const auto frames = static_cast<uint64_t>(std::max(0.0, path.duration() * rate));
    if (mode == WarpMode::Off || mode == WarpMode::Repitch)
        return resampleAlong(framesOf(source), rate, path, frames, cancel);

    // A transpose: the stretch made `pitch` times longer (its own path), then read `pitch` times faster
    pitch = std::clamp(std::isfinite(pitch) ? pitch : 1.0, 0.25, 4.0);
    const bool shifted = std::fabs(pitch - 1.0) > 1e-6;
    TimePath stretched = path;
    uint64_t stretchedFrames = frames;
    if (shifted) {
        stretched.step = path.step * pitch;
        stretchedFrames = static_cast<uint64_t>(std::ceil(static_cast<double>(frames) * pitch)) + 4;
    }
    std::vector<float> y = mode == WarpMode::Tonal ? tonal(source, stretched, stretchedFrames, transients, cancel)
                                                   : beats(source, stretched, stretchedFrames, transients, cancel);
    if (!shifted || y.empty())
        return y;
    if (pitch > 1.0)
        lowPass(y, source.channels, 0.45 / pitch);
    TimePath faster;
    faster.step = 1.0;
    faster.source = {0.0, pitch};
    const Frames yFrames{y.data(), static_cast<int64_t>(y.size() / source.channels), source.channels};
    return resampleAlong(yFrames, rate, faster, frames, cancel);
}

}
