#include "WarpSelfTest.h"
#include "Harness.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "audio/AudioAnalysis.h"
#include "audio/AudioFile.h"
#include "audio/ClipPlayer.h"
#include "audio/Fft.h"
#include "audio/TempoMap.h"
#include "audio/Warp.h"
#include "daw/app/Appearance.h"
#include "daw/io/ProjectFile.h"
#include "daw/model/History.h"
#include "daw/model/Project.h"
#include "daw/model/ProjectStore.h"
#include "daw/model/Takes.h"
#include "daw/model/Warp.h"
#include "daw/views/AudioEditor.h"

namespace tests {

namespace {

constexpr double kRate = 48000.0;
constexpr double kPi = 3.141592653589793;

std::string number(const char *pattern, double value)
{
    char buffer[96];
    std::snprintf(buffer, sizeof(buffer), pattern, value);
    return buffer;
}

bool near(double a, double b, double tolerance = 1e-9)
{
    return std::fabs(a - b) <= tolerance;
}

struct Output {
    std::vector<float> left, right;
};

// Plays a player for `seconds` from `position` (advanced), in blocks, under `map`
void run(audio::AudioClipPlayer &player, const audio::TempoMap &map, uint64_t &position, double seconds, Output &out,
         uint32_t blockFrames = 256)
{
    const auto frames = static_cast<uint64_t>(seconds * kRate);
    std::vector<float> l(blockFrames), r(blockFrames);
    float *channels[2] = {l.data(), r.data()};
    for (uint64_t done = 0; done < frames;) {
        const auto n = static_cast<uint32_t>(std::min<uint64_t>(blockFrames, frames - done));
        std::fill(l.begin(), l.end(), 0.0f);
        std::fill(r.begin(), r.end(), 0.0f);
        audio::TransportState t;
        t.playing = true;
        t.position = position;
        t.sampleRate = kRate;
        t.map = &map;
        t.tempo = map.bpmAt(0.0);
        audio::AudioBlock block{channels, 2, n, kRate, t};
        player.process(block);
        out.left.insert(out.left.end(), l.begin(), l.begin() + n);
        out.right.insert(out.right.end(), r.begin(), r.begin() + n);
        position += n;
        done += n;
    }
    player.collect();
}

void send(audio::AudioClipPlayer &player, std::vector<audio::ClipRegion> regions)
{
    auto sequence = std::make_unique<audio::ClipSequence>();
    sequence->regions = std::move(regions);
    sequence->sort();
    player.setSequence(std::move(sequence));
}

// A sine, mono, `seconds` long
std::shared_ptr<audio::AudioData> sine(double hz, double seconds, float level = 0.5f)
{
    std::vector<float> s(static_cast<size_t>(seconds * kRate));
    for (size_t i = 0; i < s.size(); ++i)
        s[i] = level * static_cast<float>(std::sin(2.0 * kPi * hz * static_cast<double>(i) / kRate));
    return audio::makeAudioData("sine", kRate, 1, std::move(s));
}

// Clicks every `every` seconds: a short burst of a 3 kHz tone dying away in 8 ms
std::vector<float> clickTrain(double seconds, double every)
{
    std::vector<float> s(static_cast<size_t>(seconds * kRate), 0.0f);
    for (double at = 0.0; at < seconds - 0.05; at += every) {
        const auto first = static_cast<size_t>(std::llround(at * kRate));
        for (size_t i = 0; i < 1200 && first + i < s.size(); ++i) {
            const double t = static_cast<double>(i) / kRate;
            s[first + i] = static_cast<float>(0.9 * std::exp(-t / 0.008) * std::sin(2.0 * kPi * 3000.0 * t));
        }
    }
    return s;
}

// Upward zero crossings of a channel in [from, to) seconds, as a frequency
double frequency(const std::vector<float> &x, double from, double to)
{
    const auto a = static_cast<size_t>(from * kRate);
    const auto b = std::min(x.size(), static_cast<size_t>(to * kRate));
    int crossings = 0;
    for (size_t i = a + 1; i < b; ++i)
        crossings += x[i - 1] < 0.0f && x[i] >= 0.0f ? 1 : 0;
    return crossings / (static_cast<double>(b - a) / kRate);
}

double rms(const std::vector<float> &x, double from, double to)
{
    const auto a = static_cast<size_t>(from * kRate);
    const auto b = std::min(x.size(), static_cast<size_t>(to * kRate));
    double sum = 0.0;
    for (size_t i = a; i < b; ++i)
        sum += static_cast<double>(x[i]) * x[i];
    return b > a ? std::sqrt(sum / static_cast<double>(b - a)) : 0.0;
}

// The starts of the hits in a signal: where it first passes `threshold` after `quiet` seconds below it
std::vector<double> onsets(const std::vector<float> &x, float threshold = 0.3f, double quiet = 0.2)
{
    std::vector<double> found;
    double last = -1e9;
    for (size_t i = 0; i < x.size(); ++i) {
        if (std::fabs(x[i]) < threshold)
            continue;
        const double t = static_cast<double>(i) / kRate;
        if (t - last > quiet)
            found.push_back(t);
        last = t;
    }
    return found;
}

audio::ClipRegion region(std::shared_ptr<const audio::AudioData> data, double start, double end, uint64_t key = 1)
{
    audio::ClipRegion r;
    r.start = start;
    r.end = end;
    r.data = std::move(data);
    r.key = key;
    return r;
}

// A region warped at `bpm` with no markers, its content starting at the file's start
audio::ClipRegion warped(std::shared_ptr<const audio::AudioData> data, double start, double end, double bpm,
                         audio::WarpMode mode, double pitch = 0.0)
{
    audio::ClipRegion r = region(std::move(data), start, end);
    r.warp.on = true;
    r.warp.bpm = bpm;
    r.warp.mode = mode;
    r.warp.pitch = pitch;
    r.contentStart = 0.0;
    return r;
}

}

int runWarpAudioChecks()
{
    Checker c;

    // The FFT: back again to what went in, and a sine's energy in its bin
    {
        audio::Fft fft(2048);
        std::vector<float> in(2048), re(fft.bins()), im(fft.bins()), back(2048);
        uint32_t seed = 7;
        for (float &v : in) {
            seed = seed * 1664525u + 1013904223u;
            v = static_cast<float>(seed >> 8) / 16777216.0f - 0.5f;
        }
        fft.forward(in.data(), re.data(), im.data());
        fft.inverse(re.data(), im.data(), back.data());
        float error = 0.0f;
        for (size_t i = 0; i < in.size(); ++i)
            error = std::max(error, std::fabs(in[i] - back[i]));
        for (size_t i = 0; i < in.size(); ++i)
            in[i] = static_cast<float>(std::cos(2.0 * kPi * 10.0 * static_cast<double>(i) / 2048.0));
        fft.forward(in.data(), re.data(), im.data());
        size_t peak = 0;
        for (size_t k = 0; k < fft.bins(); ++k) {
            if (std::hypot(re[k], im[k]) > std::hypot(re[peak], im[peak]))
                peak = k;
        }
        c.check(error < 1e-5f && peak == 10 && std::fabs(re[10] - 1024.0f) < 0.1f,
                "Warp: the FFT goes back to what went in, a sine in its bin", number("error %.2e", error));
    }

    // The map: lines between markers, the clip's tempo outside them, and back
    {
        audio::Warp w;
        w.bpm = 120.0;
        const bool none = near(audio::warpSeconds(w, 4.0), 2.0) && near(audio::warpBeat(w, 3.0), 6.0);
        w.markers = {{0.5, 1.0}};
        const bool one = near(audio::warpSeconds(w, 3.0), 1.5) && near(audio::warpBeat(w, 0.0), 0.0);
        w.markers = {{0.0, 0.0}, {2.0, 2.0}, {3.0, 4.0}};
        const bool many = near(audio::warpSeconds(w, 1.0), 1.0) && near(audio::warpSeconds(w, 3.0), 2.5) &&
                          near(audio::warpSeconds(w, 6.0), 4.0) && near(audio::warpSeconds(w, -2.0), -1.0) &&
                          near(audio::warpBeat(w, 2.5), 3.0) && near(audio::warpBeat(w, 4.0), 6.0) &&
                          near(audio::warpSecondsPerBeat(w, 1.0), 1.0) && near(audio::warpSecondsPerBeat(w, 3.0), 0.5);
        c.check(none && one && many, "Warp: the map joins markers by lines, the clip's tempo outside them, both ways");
        audio::Warp messy;
        messy.bpm = 5000.0;
        messy.pitch = 99.0;
        messy.markers = {{2.0, 2.0}, {1.0, 3.0}, {0.0, 0.0}, {3.0, 4.0}};
        audio::tidyWarp(messy);
        c.check(messy.bpm == audio::kMaxWarpBpm && messy.pitch == audio::kMaxWarpPitch && messy.markers.size() == 3 &&
                    messy.markers[1].beat == 2.0,
                "Warp: tidying clamps it and drops a marker that runs backwards",
                number("%.0f markers kept", static_cast<double>(messy.markers.size())));
    }

    // The analysis: a click train at 120 BPM, 8 seconds (4 bars: a loop)
    std::vector<float> clicks = clickTrain(8.0, 0.5);
    {
        const audio::AudioAnalysis a = audio::analyseSamples(clicks.data(), clicks.size(), 1, kRate);
        double worst = 0.0;
        size_t matched = 0;
        for (const audio::Transient &t : a.transients) {
            const double s = static_cast<double>(t.frame) / kRate;
            const double nearest = std::round(s / 0.5) * 0.5;
            worst = std::max(worst, std::fabs(s - nearest));
            matched += std::fabs(s - nearest) < 0.002 ? 1 : 0;
        }
        if (flag("VYBN_FLUX")) {
            for (const audio::Transient &t : a.transients)
                std::printf("INFO  transient %llu %.2f\n", static_cast<unsigned long long>(t.frame), t.strength);
        }
        c.check(a.transients.size() == 16 && matched == 16, "Warp: the analysis finds a click train's clicks within 2 ms",
                number("%.0f found", static_cast<double>(a.transients.size())) + number(", worst %.2f ms", worst * 1000.0));
        c.check(a.loop && a.loopBars == 4 && near(a.bpm, 120.0, 1e-6) && near(a.downbeat, 0.0, 0.002),
                "Warp: it is a loop of 4 bars at 120 BPM, its first beat at its start",
                number("%.2f BPM", a.bpm) + number(", %.0f bars", a.loopBars));
        c.check(daw::guessMode(a) == audio::WarpMode::Beats, "Warp: clicks are percussive (Beats)",
                number("percussive %.2f", a.percussive));
        std::vector<float> pad(static_cast<size_t>(6.0 * kRate));
        for (size_t i = 0; i < pad.size(); ++i) {
            const double t = static_cast<double>(i) / kRate;
            pad[i] = static_cast<float>(0.3 * std::sin(2.0 * kPi * 220.0 * t) + 0.2 * std::sin(2.0 * kPi * 331.0 * t)) *
                     static_cast<float>(std::min(1.0, t / 0.5));
        }
        const audio::AudioAnalysis held = audio::analyseSamples(pad.data(), pad.size(), 1, kRate);
        c.check(daw::guessMode(held) == audio::WarpMode::Complex && held.transients.size() <= 2,
                "Warp: a held chord isn't percussive (Complex)",
                number("percussive %.2f", held.percussive) + number(", %.0f transients", static_cast<double>(held.transients.size())));
        std::vector<float> faster = clickTrain(6.0, 60.0 / 97.0);
        const audio::AudioAnalysis tempo = audio::analyseSamples(faster.data(), faster.size(), 1, kRate);
        c.check(near(tempo.bpm, 97.0, 1.0), "Warp: the tempo of clicks at 97 BPM", number("%.1f BPM", tempo.bpm));
    }

    const audio::TempoMap at120(120.0);
    const audio::TempoMap at80(80.0);
    const audio::TempoMap at90(90.0);

    // At the file's own speed and pitch every mode reads the file as an unwarped clip does
    {
        std::vector<float> ramp(static_cast<size_t>(4.0 * kRate));
        for (size_t i = 0; i < ramp.size(); ++i)
            ramp[i] = static_cast<float>(i) / static_cast<float>(kRate) * 0.2f;
        auto data = audio::makeAudioData("ramp", kRate, 1, ramp);
        audio::AudioClipPlayer plain;
        audio::ClipRegion r = region(data, 1.0, 5.0);
        r.offset = 0.25;
        send(plain, {r});
        Output expected;
        uint64_t pos = 0;
        run(plain, at120, pos, 2.6, expected);
        float worst = 0.0f;
        for (audio::WarpMode mode : {audio::WarpMode::Repitch, audio::WarpMode::Beats, audio::WarpMode::Tones, audio::WarpMode::Complex}) {
            audio::AudioClipPlayer player;
            audio::ClipRegion w = r;
            w.warp.on = true;
            w.warp.bpm = 120.0;
            w.warp.mode = mode;
            w.contentStart = audio::warpBeat(w.warp, r.offset);
            w.slices = std::make_shared<const std::vector<audio::WarpSlice>>(std::vector<audio::WarpSlice>{{0.0, 0.0}});
            send(player, {w});
            Output got;
            uint64_t p = 0;
            run(player, at120, p, 2.6, got);
            for (size_t i = 0; i < expected.left.size(); ++i)
                worst = std::max(worst, std::fabs(expected.left[i] - got.left[i]));
        }
        c.check(worst < 1e-5f, "Warp: at its own speed every mode plays the file sample for sample", number("worst %.2e", worst));
    }

    // Repitch: a file at 60 BPM on a 120 BPM timeline plays twice as fast
    {
        std::vector<float> ramp(static_cast<size_t>(4.0 * kRate));
        for (size_t i = 0; i < ramp.size(); ++i)
            ramp[i] = static_cast<float>(i) / static_cast<float>(kRate) * 0.2f;
        audio::AudioClipPlayer player;
        send(player, {warped(audio::makeAudioData("ramp", kRate, 1, ramp), 0.0, 8.0, 60.0, audio::WarpMode::Repitch)});
        Output out;
        uint64_t pos = 0;
        run(player, at120, pos, 1.0, out);
        const double at = out.left[static_cast<size_t>(0.5 * kRate)] / 0.2;
        c.check(near(at, 1.0, 1e-3), "Warp: Repitch at twice the speed reads two seconds a second", number("%.4f s into the file", at));
    }

    // Tones and Complex: a 440 Hz sine stretched by 1.5 (a 120 BPM file at 80 BPM) stays at 440 Hz
    // and lasts half as long again; transposed an octave at its own speed it is 880 Hz as long as before
    auto tone = sine(440.0, 4.0);
    for (audio::WarpMode mode : {audio::WarpMode::Tones, audio::WarpMode::Complex}) {
        const std::string name = daw::warpModeName(mode);
        {
            audio::AudioClipPlayer player;
            send(player, {warped(tone, 0.0, 8.0, 120.0, mode)});
            Output out;
            uint64_t pos = 0;
            run(player, at80, pos, 6.5, out);
            const double hz = frequency(out.left, 1.0, 5.0);
            const double level = rms(out.left, 1.0, 5.0);
            const double tail = rms(out.left, 5.5, 5.9);
            c.check(near(hz, 440.0, 4.4) && near(level, 0.5 / std::sqrt(2.0), 0.08) && tail > 0.2 && rms(out.left, 6.1, 6.5) < 1e-6,
                    "Warp: " + name + " stretched by 1.5 keeps a sine at 440 Hz for 6 s",
                    number("%.1f Hz", hz) + number(", level %.3f", level) + number(", tail %.3f", tail));
        }
        {
            audio::AudioClipPlayer player;
            send(player, {warped(tone, 0.0, 8.0, 120.0, mode, 12.0)});
            Output out;
            uint64_t pos = 0;
            run(player, at120, pos, 4.4, out);
            const double hz = frequency(out.left, 0.5, 3.5);
            c.check(near(hz, 880.0, 13.0) && rms(out.left, 3.6, 3.9) > 0.15 && rms(out.left, 4.1, 4.4) < 1e-6,
                    "Warp: " + name + " transposed an octave is 880 Hz, as long as before", number("%.1f Hz", hz));
        }
    }

    // Beats: clicks at 120 BPM played at 90 BPM land on the new beats, each as sharp as before
    {
        auto data = audio::makeAudioData("clicks", kRate, 1, clicks);
        const audio::AudioAnalysis a = audio::analyseSamples(clicks.data(), clicks.size(), 1, kRate);
        audio::ClipRegion r = warped(data, 0.0, 16.0, 120.0, audio::WarpMode::Beats);
        daw::Clip clip("Clicks", 0.0, 16.0);
        clip.warp = r.warp;
        r.slices = std::make_shared<const std::vector<audio::WarpSlice>>(daw::warpSlices(clip, &a, kRate, data->frames));
        audio::AudioClipPlayer player;
        send(player, {r});
        Output out;
        uint64_t pos = 0;
        run(player, at90, pos, 10.8, out);
        const std::vector<double> hits = onsets(out.left);
        double worst = 0.0;
        for (size_t i = 0; i < hits.size(); ++i)
            worst = std::max(worst, std::fabs(hits[i] - static_cast<double>(i) * 60.0 / 90.0));
        float peak = 0.0f;
        for (float v : out.left)
            peak = std::max(peak, std::fabs(v));
        c.check(hits.size() == 16 && worst < 0.002 && peak > 0.8f, "Warp: Beats puts each click on its beat at 90 BPM",
                number("%.0f hits", static_cast<double>(hits.size())) + number(", worst %.2f ms", worst * 1000.0));
        // Played faster, at 160 BPM, the clicks still land
        const audio::TempoMap at160(160.0);
        audio::AudioClipPlayer fast;
        send(fast, {r});
        Output quick;
        pos = 0;
        run(fast, at160, pos, 6.0, quick);
        const std::vector<double> quickHits = onsets(quick.left, 0.3f, 0.15);
        double worstQuick = 0.0;
        for (size_t i = 0; i < quickHits.size(); ++i)
            worstQuick = std::max(worstQuick, std::fabs(quickHits[i] - static_cast<double>(i) * 60.0 / 160.0));
        c.check(quickHits.size() == 16 && worstQuick < 0.002, "Warp: and at 160 BPM",
                number("%.0f hits", static_cast<double>(quickHits.size())) + number(", worst %.2f ms", worstQuick * 1000.0));
    }

    // A voice stays with its clip across a new sequence: the sound goes on exactly as without one
    {
        audio::ClipRegion r = warped(tone, 0.0, 8.0, 120.0, audio::WarpMode::Complex);
        audio::AudioClipPlayer once;
        send(once, {r});
        Output straight;
        uint64_t pos = 0;
        run(once, at80, pos, 2.0, straight);
        audio::AudioClipPlayer swapped;
        send(swapped, {r});
        Output twice;
        pos = 0;
        run(swapped, at80, pos, 1.0, twice);
        send(swapped, {r});
        run(swapped, at80, pos, 1.0, twice);
        float worst = 0.0f;
        for (size_t i = 0; i < straight.left.size() && i < twice.left.size(); ++i)
            worst = std::max(worst, std::fabs(straight.left[i] - twice.left[i]));
        c.check(worst < 1e-6f && swapped.voiceCount() == 1, "Warp: a new sequence keeps the clip's voice going",
                number("worst %.2e", worst));
        // Two stretched clips at once take two voices; a third sequence like them makes none
        audio::ClipRegion other = r;
        other.key = 2;
        send(swapped, {r, other});
        run(swapped, at80, pos, 0.1, twice);
        send(swapped, {r, other});
        run(swapped, at80, pos, 0.1, twice);
        c.check(swapped.voiceCount() == 2, "Warp: two clips stretching at once have a voice each",
                number("%.0f voices", static_cast<double>(swapped.voiceCount())));
        // A jump starts the voice again where it lands, at once
        Output jumped;
        uint64_t far = static_cast<uint64_t>(3.0 * kRate);
        send(swapped, {r});
        run(swapped, at80, far, 0.5, jumped);
        c.check(rms(jumped.left, 0.0, 0.01) > 0.25, "Warp: after a jump the stretched sound is there at once",
                number("level %.3f in the first 10 ms", rms(jumped.left, 0.0, 0.01)));
    }

    // Every mode plays the same in big blocks as in small ones (an export renders 4096 frames at a time)
    {
        auto data = audio::makeAudioData("clicks", kRate, 1, clicks);
        const audio::AudioAnalysis a = audio::analyseSamples(clicks.data(), clicks.size(), 1, kRate);
        float worst = 0.0f;
        for (audio::WarpMode mode : {audio::WarpMode::Beats, audio::WarpMode::Tones, audio::WarpMode::Complex}) {
            audio::ClipRegion r = warped(data, 0.0, 16.0, 120.0, mode, 2.0);
            daw::Clip clip("Clicks", 0.0, 16.0);
            clip.warp = r.warp;
            r.slices = std::make_shared<const std::vector<audio::WarpSlice>>(daw::warpSlices(clip, &a, kRate, data->frames));
            Output small, big;
            for (uint32_t block : {256u, 4096u}) {
                audio::AudioClipPlayer player;
                send(player, {r});
                uint64_t pos = 0;
                run(player, at80, pos, 3.0, block == 256u ? small : big, block);
            }
            for (size_t i = 0; i < small.left.size(); ++i)
                worst = std::max(worst, std::fabs(small.left[i] - big.left[i]));
        }
        c.check(worst < 1e-4f, "Warp: every mode plays the same in blocks of 4096 as of 256", number("worst %.2e", worst));
    }

    // The cost of each mode: ten seconds of a stereo clip stretched by 1.5 and transposed, in 256-frame blocks
    if (bench()) {
        std::vector<float> stereo(static_cast<size_t>(8.0 * kRate) * 2);
        for (size_t i = 0; i < stereo.size() / 2; ++i) {
            const double t = static_cast<double>(i) / kRate;
            stereo[i * 2] = static_cast<float>(0.3 * std::sin(2.0 * kPi * 220.0 * t) + 0.1 * std::sin(2.0 * kPi * 1500.0 * t));
            stereo[i * 2 + 1] = static_cast<float>(0.3 * std::sin(2.0 * kPi * 330.0 * t));
        }
        auto data = audio::makeAudioData("bench", kRate, 2, stereo);
        for (audio::WarpMode mode : {audio::WarpMode::Repitch, audio::WarpMode::Beats, audio::WarpMode::Tones, audio::WarpMode::Complex}) {
            audio::ClipRegion r = warped(data, 0.0, 16.0, 120.0, mode, 2.0);
            daw::Clip clip("Bench", 0.0, 16.0);
            clip.warp = r.warp;
            r.slices = std::make_shared<const std::vector<audio::WarpSlice>>(daw::warpSlices(clip, nullptr, kRate, data->frames));
            audio::AudioClipPlayer player;
            send(player, {r});
            Output out;
            uint64_t pos = 0;
            const auto start = std::chrono::steady_clock::now();
            run(player, at80, pos, 10.0, out);
            const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
            std::printf("INFO  warp bench: %s, 10 s of stereo stretched by 1.5: %.1f ms (%.2f%% of real time)\n",
                        daw::warpModeName(mode), ms, ms / 100.0);
        }
    }

    // A tempo change while a stretched clip plays: it follows (the beats move under it)
    {
        audio::TempoMap change(120.0);
        change.setPoints({{0.0, 120.0, false}, {4.0, 60.0, false}});
        audio::AudioClipPlayer player;
        send(player, {warped(tone, 0.0, 8.0, 120.0, audio::WarpMode::Complex)});
        Output out;
        uint64_t pos = 0;
        run(player, change, pos, 6.5, out);
        // Beats 0-4 take 2 s, beats 4-8 at 60 BPM take 4 s: the clip ends at 6 s
        c.check(near(frequency(out.left, 0.5, 5.5), 440.0, 4.4) && rms(out.left, 5.6, 5.9) > 0.15 && rms(out.left, 6.1, 6.4) < 1e-6,
                "Warp: a warped clip follows a tempo change under it", number("%.1f Hz", frequency(out.left, 0.5, 5.5)));
    }

    return c.failures;
}

int runWarpModelChecks()
{
    Checker c;
    using daw::Clip;

    audio::TempoMap timing(120.0);
    // A clip at beat 4 playing a 10 s file from 0.5 s in
    Clip base("Loop", 4.0, 8.0);
    base.id = 5;
    base.audioFile = "loop.wav";
    base.offset = 0.5;
    const double fileSeconds = 10.0;

    // Turning warp on keeps the sound where it is, under a tempo map with a change and a ramp
    {
        audio::TempoMap map(120.0);
        map.setPoints({{0.0, 120.0, false}, {6.0, 90.0, true}, {10.0, 140.0, false}});
        Clip w = base;
        Clip plain = base;
        daw::setWarp(w, map, true, fileSeconds);
        double worst = 0.0;
        for (double b = 2.0; b < 20.0; b += 0.37)
            worst = std::max(worst, std::fabs(daw::clipSourceSeconds(w, map, b) - daw::clipSourceSeconds(plain, map, b)));
        c.check(w.warp.on && w.warp.markers.size() > 2 && worst < 2e-3,
                "Warp: turned on, a clip sounds where it did through tempo changes and a ramp",
                number("worst %.2e s", worst) + number(", %.0f markers", static_cast<double>(w.warp.markers.size())));
        Clip flat = base;
        daw::setWarp(flat, timing, true, fileSeconds);
        double flatWorst = 0.0;
        for (double b = 4.0; b < 12.0; b += 0.5)
            flatWorst = std::max(flatWorst, std::fabs(daw::clipSourceSeconds(flat, timing, b) - daw::clipSourceSeconds(base, timing, b)));
        c.check(flat.warp.markers.size() == 1 && flat.warp.bpm == 120.0 && flatWorst < 1e-9,
                "Warp: at one tempo it takes that tempo and one marker", number("worst %.2e", flatWorst));
        // Off again: the clip's start and its moment of the file stay; the length is cut to the file
        Clip off = flat;
        off.length = 100.0;
        off.warp.on = true;
        daw::setWarp(off, timing, false, fileSeconds);
        c.check(!off.warp.on && off.warp.bpm == 120.0 && near(off.length, 19.0) && near(off.offset, 0.5),
                "Warp: turned off, it keeps its start and is cut to its file", number("length %.2f", off.length));
    }

    // The clip's tempo: doubling it plays its sound in half the beats from the same start
    {
        Clip w = base;
        daw::setWarp(w, timing, true, fileSeconds);
        const double before = daw::clipSourceSeconds(w, timing, w.start);
        daw::setClipTempo(w, 240.0);
        c.check(near(w.length, 16.0) && near(daw::clipSourceSeconds(w, timing, w.start), before) &&
                    near(daw::clipSourceSeconds(w, timing, w.start + 4.0), before + 1.0),
                "Warp: doubling the clip's tempo doubles its length, keeping its start",
                number("%.2f beats", w.length));
        Clip fit = base;
        daw::setWarp(fit, timing, true, fileSeconds);
        daw::fitToLength(fit, 4.0);
        c.check(near(fit.length, 4.0) && near(fit.warp.bpm, 60.0) && near(daw::clipSourceSeconds(fit, timing, fit.end()), 4.5),
                "Warp: stretching a clip to half its length halves its tempo, its sound all there",
                number("%.1f BPM", fit.warp.bpm));
    }

    // Markers: added where the sound is (nothing moves); moved (the sound between stretches, the
    // start stays); slipped; removed
    {
        Clip w = base;
        daw::setWarp(w, timing, true, fileSeconds);
        const Clip was = w;
        const int i = daw::addWarpMarker(w, 8.0);
        double moved = 0.0;
        for (double b = 4.0; b < 12.0; b += 0.25)
            moved = std::max(moved, std::fabs(daw::clipSourceSeconds(w, timing, b) - daw::clipSourceSeconds(was, timing, b)));
        c.check(i >= 0 && w.warp.markers.size() == 2 && moved < 1e-9, "Warp: a marker added moves nothing");
        const double at8 = daw::clipSourceSeconds(w, timing, 8.0); // the file's moment the marker holds
        const int j = daw::moveWarpMarker(w, i, 9.0);
        const double startNow = daw::clipSourceSeconds(w, timing, w.start);
        c.check(j >= 0 && near(daw::clipSourceSeconds(w, timing, 9.0), at8, 1e-9) && near(startNow, 0.5, 1e-9) &&
                    near(daw::clipSourceSeconds(w, timing, 11.0), at8 + 1.0, 1e-9),
                "Warp: a marker dragged to beat 9 brings its sound there, the start staying",
                number("%.3f s at beat 9", daw::clipSourceSeconds(w, timing, 9.0)));
        const int k = daw::slipWarpMarker(w, j, at8 + 0.25);
        c.check(k == j && near(daw::clipSourceSeconds(w, timing, 9.0), at8 + 0.25, 1e-9),
                "Warp: slipping a marker plays later sound on its beat");
        const size_t count = w.warp.markers.size();
        daw::removeWarpMarker(w, k);
        c.check(w.warp.markers.size() == count - 1, "Warp: a marker removed");
        daw::clearWarpMarkers(w);
        c.check(w.warp.markers.size() == 1 && near(daw::clipSourceSeconds(w, timing, w.start), 0.5),
                "Warp: clearing keeps one marker at the clip's start");
    }

    // Audio quantise: hits a little early and late are moved onto the beats
    {
        Clip w("Drums", 0.0, 4.0);
        w.audioFile = "drums.wav";
        audio::AudioAnalysis a;
        a.sampleRate = kRate;
        a.frames = static_cast<uint64_t>(4.0 * kRate);
        for (double s : {0.52, 0.97, 1.53})
            a.transients.push_back({static_cast<uint64_t>(s * kRate), 0.9f});
        a.transients.push_back({static_cast<uint64_t>(1.25 * kRate), 0.1f}); // too weak
        const int made = daw::quantiseAudio(w, timing, a, 1.0, 1.0, 0.3f);
        double worst = 0.0;
        for (int beat = 1; beat <= 3; ++beat) {
            const double s = beat == 1 ? 0.52 : beat == 2 ? 0.97 : 1.53;
            worst = std::max(worst, std::fabs(daw::clipBeatAt(w, timing, s) - beat));
        }
        c.check(made == 3 && w.warp.on && worst < 1e-6, "Warp: quantising audio puts each hit on its beat",
                number("%.0f markers made", made) + number(", worst %.2e beats", worst));
        Clip half("Drums", 0.0, 4.0);
        half.audioFile = "drums.wav";
        daw::quantiseAudio(half, timing, a, 1.0, 0.5, 0.3f);
        c.check(near(daw::clipBeatAt(half, timing, 0.52), 1.02, 1e-6), "Warp: at half strength halfway",
                number("beat %.3f", daw::clipBeatAt(half, timing, 0.52)));
    }

    // Guessing: a loop placed at a beat warps at its tempo with its first beat there
    {
        audio::AudioAnalysis a;
        a.sampleRate = kRate;
        a.frames = static_cast<uint64_t>(4.0 * kRate);
        a.bpm = 120.0;
        a.loop = true;
        a.downbeat = 0.1;
        Clip c1("Loop", 8.0, 1.0);
        c1.audioFile = "loop.wav";
        daw::applyGuess(c1, a);
        c.check(c1.warp.on && near(daw::clipBeatAt(c1, timing, 0.1), 8.0) && near(c1.start, 7.8) &&
                    near(c1.end(), 7.8 + 8.0),
                "Warp: a loop placed at beat 8 has its first beat there and its pickup before it",
                number("starts at %.2f", c1.start));
    }

    // Splitting, trimming and slicing a warped clip keep its sound in place
    {
        daw::Project p;
        p.tracks.clear();
        p.addTrack(daw::TrackKind::Audio);
        Clip w = base;
        daw::setWarp(w, timing, true, fileSeconds);
        daw::setClipTempo(w, 100.0); // stretched
        daw::addWarpMarker(w, 6.0);
        daw::moveWarpMarker(w, 1, 6.5);
        p.tracks[0].clips = {w};
        p.select(0, 0);
        const Clip whole = p.tracks[0].clips[0];
        p.splitClip(7.0);
        const Clip &left = p.tracks[0].clips[0];
        const Clip &right = p.tracks[0].clips[1];
        double worst = 0.0;
        for (double b = 4.0; b < 7.0; b += 0.1)
            worst = std::max(worst, std::fabs(daw::clipSourceSeconds(left, p.timing, b) - daw::clipSourceSeconds(whole, p.timing, b)));
        for (double b = 7.0; b < whole.end(); b += 0.1)
            worst = std::max(worst, std::fabs(daw::clipSourceSeconds(right, p.timing, b) - daw::clipSourceSeconds(whole, p.timing, b)));
        Clip trimmed = whole;
        p.trimStart(trimmed, 5.0);
        for (double b = 5.0; b < whole.end(); b += 0.1)
            worst = std::max(worst, std::fabs(daw::clipSourceSeconds(trimmed, p.timing, b) - daw::clipSourceSeconds(whole, p.timing, b)));
        const Clip slice = daw::sliceClip(whole, 6.0, 8.0, p.timing);
        for (double b = 6.0; b < 8.0; b += 0.1)
            worst = std::max(worst, std::fabs(daw::clipSourceSeconds(slice, p.timing, b) - daw::clipSourceSeconds(whole, p.timing, b)));
        c.check(worst < 1e-9, "Warp: split, trimmed and sliced, a warped clip's sound stays put", number("worst %.2e s", worst));
        Clip early = whole;
        p.trimStart(early, 0.0);
        c.check(near(early.start, daw::fileStartBeat(whole, p.timing)) && near(early.offset, 0.0, 1e-9),
                "Warp: trimmed back, it stops at its file's start", number("starts at %.3f", early.start));
    }

    // The file: written and read back the same; older files play unwarped; bad words refused
    {
        daw::Project p;
        p.tracks.clear();
        p.addTrack(daw::TrackKind::Audio);
        Clip w = base;
        daw::setWarp(w, timing, true, fileSeconds);
        w.warp.mode = audio::WarpMode::Beats;
        w.warp.pitch = -2.5;
        w.warp.preserve = audio::WarpPreserve::Eighth;
        w.warp.loop = audio::WarpLoop::BackAndForth;
        w.warp.envelope = 40.0f;
        w.warp.grain = 70.0f;
        daw::addWarpMarker(w, 6.0);
        daw::moveWarpMarker(w, 1, 6.25);
        p.tracks[0].clips = {w};
        Clip plain = base;
        plain.id = 6;
        plain.start = 20.0;
        p.tracks[0].clips.push_back(plain);
        const std::string text = daw::writeProject(p);
        daw::Project back;
        std::string error;
        const bool read = daw::readProject(text, back, error);
        const bool same = read && back.tracks[0].clips.size() == 2 && back.tracks[0].clips[0].warp == w.warp &&
                          !back.tracks[0].clips[1].warp.on && back.tracks[0].clips[1].warp.bpm == 0.0;
        c.check(same && text.find("mode = \"beats\"") != std::string::npos && text.find("loop = \"back and forth\"") != std::string::npos,
                "Warp: a clip's warp is written and read back the same, an unwarped clip has none", error);
        auto refuse = [&](const std::string &from, const std::string &to) {
            std::string changed = text;
            const size_t at = changed.find(from);
            if (at == std::string::npos)
                return std::string("(not found)");
            changed.replace(at, from.size(), to);
            daw::Project bad;
            std::string why;
            return daw::readProject(changed, bad, why) ? std::string() : why;
        };
        const std::string badMode = refuse("mode = \"beats\"", "mode = \"elastic\"");
        const std::string badLoop = refuse("loop = \"back and forth\"", "loop = \"sideways\"");
        c.check(badMode.find("unknown mode \"elastic\"") != std::string::npos && badLoop.find("unknown loop") != std::string::npos,
                "Warp: an unknown mode or loop is refused with why", badMode);
        if (flag("VYBN_WARP_TEXT"))
            std::printf("%s\n", text.c_str());
        std::string clamped = text;
        const size_t bpmAt = clamped.find("bpm = 1.2e+02");
        if (bpmAt != std::string::npos)
            clamped.replace(bpmAt, 13, "bpm = 5000");
        daw::Project high;
        c.check(daw::readProject(clamped, high, error) && high.tracks[0].clips[0].warp.bpm == audio::kMaxWarpBpm,
                "Warp: a tempo out of range is clamped");
        std::string old = text;
        const size_t warpAt = old.find("warp = {");
        if (warpAt != std::string::npos)
            old.erase(warpAt, old.find('\n', warpAt) - warpAt + 1);
        daw::Project older;
        c.check(daw::readProject(old, older, error) && !older.tracks[0].clips[0].warp.on,
                "Warp: a file from before warping plays its clips unwarped");
        // The saved-state check and the history see it
        daw::Project moved = back;
        moved.tracks[0].clips[0].warp.markers[1].beat += 0.5;
        daw::Project transposed = back;
        transposed.tracks[0].clips[0].warp.pitch = 3.0;
        daw::Project unwarped = back;
        unwarped.tracks[0].clips[0].warp.on = false;
        c.check(!daw::sameSaved(back, moved) && daw::describeChange(back, moved) == "Move warp marker" &&
                    daw::describeChange(back, transposed) == "Transpose clip" && daw::describeChange(back, unwarped) == "Unwarp clip" &&
                    daw::describeChange(unwarped, back) == "Warp clip",
                "Warp: undo names its edits", daw::describeChange(back, moved));
    }

    // The audio editor's marker strip: a press makes a marker and drags it; a double-click removes it
    {
        daw::Project p;
        p.tracks.clear();
        p.addTrack(daw::TrackKind::Audio);
        Clip w("Loop", 0.0, 8.0);
        w.id = 3;
        w.audioFile = "loop.wav";
        daw::setWarp(w, timing, true, 4.0);
        p.tracks[0].clips = {w};
        daw::ProjectStore store(std::move(p));
        store.select(0, 0);
        daw::AudioEditorHooks hooks;
        daw::AudioEditorView editor(store, hooks);
        const ui::Theme theme = daw::studioTheme(true);
        editor.arrange({0.0f, 0.0f, 1000.0f, 300.0f}, theme);
        double time = 10.0;
        auto frame = [&](float x, float y, bool down, bool pressed, uint8_t mods = 0) {
            ui::Input in;
            in.mouse = {x, y};
            in.down = down;
            in.pressed = pressed;
            in.released = !down;
            in.mods = mods;
            in.time = time;
            time += 0.5;
            editor.handleInput(in);
        };
        const float y = editor.markerStrip().y + editor.markerStrip().h * 0.5f;
        frame(0.0f, 0.0f, false, false); // the clip is fitted in
        const daw::Project before = store.project();
        frame(editor.xOfBeat(3.0), y, true, true);
        frame(editor.xOfBeat(3.5), y, true, false);
        frame(editor.xOfBeat(3.5), y, false, false);
        const Clip &edited = *store.project().clip();
        c.check(edited.warp.markers.size() == 2 && near(daw::clipBeatAt(edited, timing, 1.5), 3.5, 1e-4) &&
                    daw::describeChange(before, store.project()) == "Add warp marker",
                "Warp: pressing the marker strip makes a marker that drags its sound along",
                number("%.0f markers", static_cast<double>(edited.warp.markers.size())) + ", " +
                    daw::describeChange(before, store.project()));
        frame(editor.markerRect(1).x + 5.0f, y, true, true);
        frame(editor.markerRect(1).x + 5.0f, y, false, false);
        time -= 0.8; // the next press comes within a double-click of the last
        frame(editor.markerRect(1).x + 5.0f, y, true, true);
        frame(editor.markerRect(1).x + 5.0f, y, false, false);
        c.check(store.project().clip()->warp.markers.size() == 1, "Warp: double-clicking a marker removes it");
    }

    return c.failures;
}

}
