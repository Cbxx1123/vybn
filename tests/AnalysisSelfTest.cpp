#include "AnalysisSelfTest.h"
#include "Harness.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

#include "audio/Analysis.h"

namespace tests {

namespace {

constexpr double kRate = 48000.0;
constexpr double kTwoPi = 6.283185307179586;

std::string text(double v)
{
    char buffer[48];
    std::snprintf(buffer, sizeof(buffer), "%.3g", v);
    return buffer;
}

// Interleaved stereo: a sine on both sides (`right` scales the right one)
std::vector<float> sine(double frequency, double seconds, double level, double right = 1.0)
{
    const auto frames = static_cast<size_t>(seconds * kRate);
    std::vector<float> x(frames * 2);
    for (size_t i = 0; i < frames; ++i) {
        const auto v = static_cast<float>(level * std::sin(kTwoPi * frequency * static_cast<double>(i) / kRate));
        x[i * 2] = v;
        x[i * 2 + 1] = static_cast<float>(v * right);
    }
    return x;
}

// The band with the highest level, and that level
size_t loudestBand(const audio::SpectrumAnalyzer &s, float &level)
{
    const auto it = std::max_element(s.bands().begin(), s.bands().end());
    level = *it;
    return static_cast<size_t>(it - s.bands().begin());
}

}

int runAnalysisChecks()
{
    Checker c;

    // The tap: frames in order, nothing while off, a full ring drops the rest
    {
        audio::AnalysisTap tap;
        std::vector<float> left(100), right(100);
        for (int i = 0; i < 100; ++i) {
            left[static_cast<size_t>(i)] = static_cast<float>(i);
            right[static_cast<size_t>(i)] = static_cast<float>(-i);
        }
        tap.write(left.data(), right.data(), 100); // off: nothing
        std::vector<float> out(400);
        const bool quiet = tap.read(out.data(), 200) == 0;
        tap.enabled = true;
        tap.write(left.data(), right.data(), 100);
        tap.write(left.data(), right.data(), 50);
        const size_t got = tap.read(out.data(), 200);
        bool ordered = got == 150;
        for (size_t i = 0; ordered && i < 150; ++i)
            ordered = out[i * 2] == static_cast<float>(i % 100) && out[i * 2 + 1] == -static_cast<float>(i % 100);
        std::vector<float> lots(audio::AnalysisTap::kFrames + 10, 0.5f);
        tap.write(lots.data(), lots.data(), static_cast<uint32_t>(lots.size()));
        const bool full = tap.dropped() == 10;
        tap.clear();
        c.check(quiet && ordered && full && tap.read(out.data(), 10) == 0, "Analysis: a tap passes frames in order, drops what won't fit",
                std::to_string(got) + " read, " + std::to_string(tap.dropped()) + " dropped");
    }

    // Two writers at once: one writes, the other skips its block, and what arrives is whole blocks
    {
        audio::AnalysisTap tap;
        tap.enabled = true;
        std::atomic<bool> go{false};
        auto writer = [&](float value) {
            std::vector<float> block(64, value);
            while (!go.load()) {
            }
            for (int k = 0; k < 2000; ++k)
                tap.write(block.data(), block.data(), 64);
        };
        std::thread a(writer, 1.0f), b(writer, 2.0f);
        go = true;
        std::vector<float> out(audio::AnalysisTap::kFrames * 2);
        size_t total = 0;
        bool whole = true;
        for (int round = 0; round < 400 && whole; ++round) {
            const size_t got = tap.read(out.data(), audio::AnalysisTap::kFrames);
            for (size_t i = 0; i < got; ++i)
                whole = whole && (out[i * 2] == 1.0f || out[i * 2] == 2.0f) && out[i * 2] == out[i * 2 + 1];
            total += got;
            std::this_thread::yield();
        }
        a.join();
        b.join();
        total += tap.read(out.data(), audio::AnalysisTap::kFrames);
        c.check(whole && total + tap.dropped() + tap.skipped() * 64 == 4000 * 64, "Analysis: a second writer skips its block, nothing tears",
                std::to_string(tap.skipped()) + " skipped");
    }

    // The spectrum of a 1 kHz sine at -6 dBFS: its band, at its level
    {
        audio::SpectrumAnalyzer s(8192);
        const std::vector<float> x = sine(1000.0, 0.5, 0.5);
        s.push(x.data(), x.size() / 2);
        s.update(kRate, 0.02);
        float level = 0.0f;
        const size_t band = loudestBand(s, level);
        const double f = audio::SpectrumAnalyzer::bandFrequency(band);
        c.check(std::fabs(f / 1000.0 - 1.0) < 0.03 && std::fabs(level + 6.02f) < 1.0f, "Analysis: a 1 kHz sine's band and level",
                text(f) + " Hz, " + text(level) + " dB");
        // A tilt of 4.5 dB an octave lifts 4 kHz by 9 dB and leaves 1 kHz
        audio::SpectrumAnalyzer tilted(8192);
        tilted.setTilt(4.5f);
        const std::vector<float> high = sine(4000.0, 0.5, 0.5);
        tilted.push(high.data(), high.size() / 2);
        tilted.update(kRate, 0.02);
        float lifted = 0.0f;
        loudestBand(tilted, lifted);
        c.check(std::fabs(lifted - (-6.02f + 9.0f)) < 1.0f, "Analysis: the tilt lifts the highs by its slope", text(lifted) + " dB");
        // Silence after it: the band falls at its rate and the peak holds
        const std::vector<float> silence(static_cast<size_t>(kRate) * 2, 0.0f);
        s.push(silence.data(), silence.size() / 2);
        s.update(kRate, 0.5);
        const float fallen = s.bands()[band];
        c.check(std::fabs(fallen - (level - 10.0f)) < 0.5f && s.peaks()[band] == level,
                "Analysis: a band falls 20 dB a second, its peak holds", text(fallen) + " dB");
    }

    // The stereo meter: mono +1, inverted -1, unrelated noise about 0; balance follows the louder side
    {
        audio::StereoMeter mono, inverted, noise, right;
        const std::vector<float> same = sine(440.0, 1.0, 0.5);
        const std::vector<float> flipped = sine(440.0, 1.0, 0.5, -1.0);
        const std::vector<float> loudRight = sine(440.0, 1.0, 0.5, 3.0);
        std::vector<float> random(static_cast<size_t>(kRate) * 2);
        uint32_t seed = 5;
        for (float &v : random) {
            seed = seed * 1664525u + 1013904223u;
            v = static_cast<float>(static_cast<double>(seed >> 8) / 16777216.0 - 0.5);
        }
        mono.push(same.data(), same.size() / 2, kRate);
        inverted.push(flipped.data(), flipped.size() / 2, kRate);
        noise.push(random.data(), random.size() / 2, kRate);
        right.push(loudRight.data(), loudRight.size() / 2, kRate);
        c.check(mono.correlation() > 0.99f && inverted.correlation() < -0.99f && std::fabs(noise.correlation()) < 0.1f &&
                    mono.width() < 0.01f && right.balance() > 0.7f && std::fabs(mono.balance()) < 0.01f &&
                    mono.points().size() == audio::StereoMeter::kPoints * 2,
                "Analysis: correlation, width and balance",
                text(mono.correlation()) + " / " + text(inverted.correlation()) + " / " + text(noise.correlation()));
    }

    return c.failures;
}

}
