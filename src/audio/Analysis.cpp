#include "Analysis.h"

#include <algorithm>
#include <cmath>

namespace audio {

namespace {

constexpr double kLowest = 20.0, kHighest = 20000.0;

}

// The tap

AnalysisTap::AnalysisTap() : ring_(new float[static_cast<size_t>(kFrames) * 2]()) {}

bool AnalysisTap::begin(uint32_t frames, uint64_t &head, uint32_t &fits)
{
    if (!enabled.load(std::memory_order_relaxed))
        return false;
    if (writing_.exchange(true, std::memory_order_acquire)) {
        skipped_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    head = head_.load(std::memory_order_relaxed);
    const uint64_t tail = tail_.load(std::memory_order_acquire);
    const auto room = static_cast<uint32_t>(kFrames - (head - tail));
    fits = std::min(frames, room);
    if (fits < frames)
        dropped_.fetch_add(frames - fits, std::memory_order_relaxed);
    return true;
}

void AnalysisTap::end(uint64_t head, uint32_t written)
{
    head_.store(head + written, std::memory_order_release);
    writing_.store(false, std::memory_order_release);
}

void AnalysisTap::write(const float *left, const float *right, uint32_t frames)
{
    uint64_t head = 0;
    uint32_t fits = 0;
    if (!begin(frames, head, fits))
        return;
    for (uint32_t i = 0; i < fits; ++i) {
        const size_t at = static_cast<size_t>((head + i) & (kFrames - 1)) * 2;
        ring_[at] = left[i];
        ring_[at + 1] = right[i];
    }
    end(head, fits);
}

void AnalysisTap::writeInterleaved(const float *stereo, uint32_t frames)
{
    uint64_t head = 0;
    uint32_t fits = 0;
    if (!begin(frames, head, fits))
        return;
    for (uint32_t i = 0; i < fits; ++i) {
        const size_t at = static_cast<size_t>((head + i) & (kFrames - 1)) * 2;
        ring_[at] = stereo[i * 2];
        ring_[at + 1] = stereo[i * 2 + 1];
    }
    end(head, fits);
}

size_t AnalysisTap::read(float *stereo, size_t frames)
{
    const uint64_t head = head_.load(std::memory_order_acquire);
    const uint64_t tail = tail_.load(std::memory_order_relaxed);
    const auto n = static_cast<size_t>(std::min<uint64_t>(head - tail, frames));
    for (size_t i = 0; i < n; ++i) {
        const size_t at = static_cast<size_t>((tail + i) & (kFrames - 1)) * 2;
        stereo[i * 2] = ring_[at];
        stereo[i * 2 + 1] = ring_[at + 1];
    }
    tail_.store(tail + n, std::memory_order_release);
    return n;
}

void AnalysisTap::clear()
{
    tail_.store(head_.load(std::memory_order_acquire), std::memory_order_release);
}

// The spectrum

SpectrumAnalyzer::SpectrumAnalyzer(size_t size) : fft_(size)
{
    setSize(size);
}

void SpectrumAnalyzer::setSize(size_t size)
{
    size = std::clamp<size_t>(size, 1024, 32768);
    fft_ = RealFft(size);
    const size_t n = fft_.size();
    window_ = hannWindow(n);
    double sum = 0.0;
    for (float w : window_)
        sum += w;
    windowGain_ = static_cast<float>(2.0 / sum); // a full-scale sine's bin reads 1
    left_.assign(n, 0.0f);
    right_.assign(n, 0.0f);
    write_ = 0;
    frame_.assign(n, 0.0f);
    re_.assign(fft_.bins(), 0.0f);
    im_.assign(fft_.bins(), 0.0f);
    power_.assign(fft_.bins(), 0.0f);
    reset();
}

void SpectrumAnalyzer::reset()
{
    bands_.assign(kBands, kFloorDb);
    peaks_.assign(kBands, kFloorDb);
    held_.assign(kBands, 0.0f);
}

double SpectrumAnalyzer::bandFrequency(size_t band)
{
    return kLowest * std::pow(kHighest / kLowest, static_cast<double>(band) / static_cast<double>(kBands - 1));
}

void SpectrumAnalyzer::push(const float *stereo, size_t frames)
{
    const size_t n = left_.size();
    for (size_t i = 0; i < frames; ++i) {
        left_[write_] = stereo[i * 2];
        right_[write_] = stereo[i * 2 + 1];
        write_ = (write_ + 1) % n;
    }
}

void SpectrumAnalyzer::update(double sampleRate, double seconds)
{
    const size_t n = left_.size();
    const size_t bins = fft_.bins();
    // The power of each bin, the channels averaged (the ring oldest first through the window)
    std::fill(power_.begin(), power_.end(), 0.0f);
    for (const std::vector<float> *channel : {&left_, &right_}) {
        for (size_t k = 0; k < n; ++k)
            frame_[k] = (*channel)[(write_ + k) % n] * window_[k];
        fft_.forward(frame_.data(), re_.data(), im_.data());
        for (size_t b = 0; b < bins; ++b)
            power_[b] += 0.5f * (re_[b] * re_[b] + im_[b] * im_[b]);
    }
    const double binHz = sampleRate / static_cast<double>(n);
    const double step = std::pow(kHighest / kLowest, 1.0 / static_cast<double>(kBands - 1));
    const double half = std::sqrt(step);
    const float gain = windowGain_ * windowGain_;
    const auto fall = static_cast<float>(fall_ * seconds);
    for (size_t i = 0; i < kBands; ++i) {
        const double f = bandFrequency(i);
        const auto lo = static_cast<size_t>(std::ceil(f / half / binHz));
        const auto hi = std::min(bins - 1, static_cast<size_t>(std::floor(f * half / binHz)));
        float p = 0.0f;
        if (hi >= lo && lo > 0) {
            for (size_t b = lo; b <= hi; ++b)
                p = std::max(p, power_[b]);
        } else {
            const double at = std::min(f / binHz, static_cast<double>(bins - 2));
            const auto b = static_cast<size_t>(at);
            const auto t = static_cast<float>(at - static_cast<double>(b));
            p = power_[b] + (power_[b + 1] - power_[b]) * t;
        }
        float db = p > 0.0f ? 10.0f * std::log10(p * gain) : kFloorDb;
        db += tilt_ * static_cast<float>(std::log2(f / 1000.0));
        db = std::max(kFloorDb, db);
        // Rising at once, falling at the fall rate; the peaks held, then falling too
        bands_[i] = std::max(db, bands_[i] - fall);
        if (bands_[i] >= peaks_[i]) {
            peaks_[i] = bands_[i];
            held_[i] = 0.0f;
        } else {
            held_[i] += static_cast<float>(seconds);
            if (hold_ >= 0.0f && held_[i] > hold_)
                peaks_[i] = std::max(bands_[i], peaks_[i] - fall);
        }
    }
}

float SpectrumAnalyzer::levelAt(double frequency) const
{
    if (frequency <= 0.0)
        return kFloorDb;
    const double place = std::log(frequency / kLowest) / std::log(kHighest / kLowest) * static_cast<double>(kBands - 1);
    const auto band = static_cast<size_t>(std::clamp(std::lround(place), 0L, static_cast<long>(kBands - 1)));
    return bands_[band];
}

// The stereo meter

void StereoMeter::push(const float *stereo, size_t frames, double sampleRate)
{
    const double a = std::exp(-1.0 / (0.3 * std::max(1.0, sampleRate)));
    const double b = 1.0 - a;
    for (size_t i = 0; i < frames; ++i) {
        const double l = stereo[i * 2], r = stereo[i * 2 + 1];
        const double m = 0.5 * (l + r), s = 0.5 * (l - r);
        lr_ = a * lr_ + b * l * r;
        ll_ = a * ll_ + b * l * l;
        rr_ = a * rr_ + b * r * r;
        mm_ = a * mm_ + b * m * m;
        ss_ = a * ss_ + b * s * s;
        ring_[write_ * 2] = static_cast<float>(l);
        ring_[write_ * 2 + 1] = static_cast<float>(r);
        write_ = (write_ + 1) % kPoints;
    }
}

void StereoMeter::reset()
{
    lr_ = ll_ = rr_ = mm_ = ss_ = 0.0;
    std::fill(ring_.begin(), ring_.end(), 0.0f);
}

float StereoMeter::correlation() const
{
    const double energy = std::sqrt(ll_ * rr_);
    return energy > 1e-12 ? static_cast<float>(std::clamp(lr_ / energy, -1.0, 1.0)) : 0.0f;
}

float StereoMeter::balance() const
{
    const double total = ll_ + rr_;
    return total > 1e-12 ? static_cast<float>((rr_ - ll_) / total) : 0.0f;
}

float StereoMeter::width() const
{
    // (all side and no mid reads as wide as it shows: 100)
    return static_cast<float>(std::min(100.0, mm_ > 1e-12 ? ss_ / mm_ : (ss_ > 1e-12 ? 100.0 : 0.0)));
}

std::vector<float> StereoMeter::points() const
{
    std::vector<float> out(kPoints * 2);
    for (size_t i = 0; i < kPoints; ++i) {
        const size_t at = (write_ + i) % kPoints;
        out[i * 2] = ring_[at * 2];
        out[i * 2 + 1] = ring_[at * 2 + 1];
    }
    return out;
}

}
