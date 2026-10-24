#include "AudioSelfTest.h"
#include "Harness.h"
#include "AnalysisSelfTest.h"
#include "WarpSelfTest.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#include "audio/AudioFile.h"
#include "audio/AudioSystem.h"
#include "audio/Effects.h"
#include "audio/Memory.h"
#include "audio/ClipPlayer.h"
#include "audio/Midi.h"
#include "audio/NotePlayer.h"
#include "audio/Recorder.h"
#include "audio/Preview.h"
#include "audio/Processors.h"
#include "audio/SampleSet.h"
#include "audio/Sampler.h"
#include "audio/Track.h"
#include "daw/app/Settings.h"
#include "env/Environment.h"
#include "env/RealtimePool.h"

namespace tests {

namespace {

constexpr double kRate = 48000.0;

std::string number(const char *pattern, double value)
{
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), pattern, value);
    return buffer;
}

// Renders `seconds` of interleaved stereo
std::vector<float> render(audio::AudioSystem &system, double seconds)
{
    std::vector<float> out(static_cast<size_t>(seconds * kRate) * audio::AudioSystem::kChannels);
    system.renderOffline(out.data(), static_cast<uint32_t>(out.size() / audio::AudioSystem::kChannels), kRate);
    return out;
}

// Peak and zero crossings of the left channel from `fromSecond` on
void measure(const std::vector<float> &samples, double fromSecond, float &peak, int &crossings)
{
    peak = 0.0f;
    crossings = 0;
    const size_t start = static_cast<size_t>(fromSecond * kRate);
    const size_t frames = samples.size() / 2;
    for (size_t i = start; i < frames; ++i) {
        const float v = samples[i * 2];
        peak = std::max(peak, std::fabs(v));
        if (i > start && (samples[(i - 1) * 2] < 0.0f) != (v < 0.0f))
            ++crossings;
    }
}

// Peaks of both channels from `fromSecond` on
void stereoPeaks(const std::vector<float> &samples, double fromSecond, float &left, float &right)
{
    left = right = 0.0f;
    for (size_t i = static_cast<size_t>(fromSecond * kRate) * 2; i + 1 < samples.size(); i += 2) {
        left = std::max(left, std::fabs(samples[i]));
        right = std::max(right, std::fabs(samples[i + 1]));
    }
}

// A track playing a 1 kHz tone at full scale on both channels
audio::TrackProcessor &toneTrack(audio::AudioSystem &system, const char *name, std::shared_ptr<audio::SoloGroup> group)
{
    auto tone = std::make_unique<audio::ToneGenerator>();
    tone->frequency.set(1000.0f);
    tone->gain.set(1.0f);
    tone->enabled = true;
    return system.add<audio::TrackProcessor>(name, std::move(group), std::move(tone));
}

}

namespace {

// A sine of `amplitude` and `frequency`, `frames` long, the same in both channels
void sine(std::vector<float> &l, std::vector<float> &r, size_t frames, double frequency, float amplitude)
{
    l.resize(frames);
    r.resize(frames);
    for (size_t i = 0; i < frames; ++i)
        l[i] = r[i] = amplitude * static_cast<float>(std::sin(6.283185307179586 * frequency * (static_cast<double>(i) / kRate)));
}

float peakFrom(const std::vector<float> &v, size_t from)
{
    float p = 0.0f;
    for (size_t i = from; i < v.size(); ++i)
        p = std::max(p, std::fabs(v[i]));
    return p;
}

float toDb(float amplitude)
{
    return amplitude > 1e-9f ? 20.0f * std::log10(amplitude) : -180.0f;
}

// An effect set up with `values` for its first parameters (the rest at their defaults)
std::unique_ptr<audio::Effect> makeConfigured(audio::EffectType type, std::initializer_list<float> values)
{
    auto effect = audio::makeEffect(type);
    effect->prepare(kRate, 4096);
    const audio::EffectInfo &info = audio::effectInfo(type);
    for (size_t i = 0; i < info.paramCount; ++i)
        effect->setParam(i, info.params[i].def);
    size_t i = 0;
    for (float v : values)
        effect->setParam(i++, v);
    return effect;
}

// Runs an effect over the buffers in pieces, as the audio thread would
void runEffect(audio::Effect &effect, std::vector<float> &l, std::vector<float> &r)
{
    for (size_t at = 0; at < l.size(); at += 512)
        effect.run(l.data() + at, r.data() + at, static_cast<uint32_t>(std::min<size_t>(512, l.size() - at)));
}

void effectChecks(Checker &c)
{
    using audio::EffectType;
    std::vector<float> l, r;
    const size_t n = static_cast<size_t>(kRate);

    // Gain
    sine(l, r, n, 1000.0, 0.25f);
    auto gain = makeConfigured(EffectType::Gain, {6.0f});
    runEffect(*gain, l, r);
    c.check(std::fabs(peakFrom(l, n / 2) - 0.25f * 1.9953f) < 0.01f, "Effects: gain of +6 dB", number("%.4f", peakFrom(l, n / 2)));
    sine(l, r, n, 1000.0, 0.25f);
    auto flip = makeConfigured(EffectType::Gain, {0.0f, 1.0f});
    runEffect(*flip, l, r);
    // Twelve samples into a cycle of a 1 kHz sine at 48 kHz is a quarter of the way through it (positive); inverted it is negative
    c.check(l[n / 2 + 12] < -0.2f, "Effects: gain inverts the phase", number("%.3f", l[n / 2 + 12]));

    // EQ: boosts the band it is set to and leaves the others
    auto eqPeak = [&](double hz, float midGain) {
        sine(l, r, n, hz, 0.1f);
        auto eq = makeConfigured(EffectType::Equalizer, {200.0f, 0.0f, 1000.0f, midGain, 1.0f, 5000.0f, 0.0f});
        runEffect(*eq, l, r);
        return peakFrom(l, n / 2) / 0.1f;
    };
    c.check(std::fabs(eqPeak(1000.0, 12.0f) - 3.98f) < 0.1f, "Effects: EQ boosts 12 dB at its frequency", number("x%.2f", eqPeak(1000.0, 12.0f)));
    c.check(std::fabs(eqPeak(100.0, 12.0f) - 1.0f) < 0.1f, "Effects: EQ leaves a distant frequency", number("x%.2f", eqPeak(100.0, 12.0f)));
    c.check(eqPeak(1000.0, -12.0f) < 0.3f, "Effects: EQ cuts 12 dB", number("x%.2f", eqPeak(1000.0, -12.0f)));

    // Filter: low-pass keeps lows and removes highs; high-pass the reverse
    auto filtered = [&](float type, double hz) {
        sine(l, r, n, hz, 0.5f);
        auto filter = makeConfigured(EffectType::Filter, {type, 1000.0f, 0.707f});
        runEffect(*filter, l, r);
        return peakFrom(l, n / 2) / 0.5f;
    };
    c.check(filtered(0.0f, 100.0) > 0.95f && filtered(0.0f, 8000.0) < 0.05f, "Effects: low-pass filter",
            number("100 Hz x%.2f", filtered(0.0f, 100.0)) + number(", 8 kHz x%.3f", filtered(0.0f, 8000.0)));
    c.check(filtered(1.0f, 100.0) < 0.05f && filtered(1.0f, 5000.0) > 0.9f, "Effects: high-pass filter",
            number("100 Hz x%.3f", filtered(1.0f, 100.0)) + number(", 5 kHz x%.2f", filtered(1.0f, 5000.0)));

    // Compressor: 4:1 over a -18 dB threshold takes a -6 dB tone down to about -15 dB, and reports it
    sine(l, r, n * 2, 440.0, 0.5f);
    auto comp = makeConfigured(EffectType::Compressor, {-18.0f, 4.0f, 1.0f, 50.0f, 0.0f, 0.0f, 100.0f});
    runEffect(*comp, l, r);
    const float compDb = toDb(peakFrom(l, n));
    c.check(std::fabs(compDb - (-15.0f)) < 0.8f, "Effects: compressor 4:1", number("%.2f dB out", compDb));
    c.check(comp->meter() > 6.0f && comp->meter() < 12.0f, "Effects: compressor reports its reduction", number("%.1f dB", comp->meter()));
    sine(l, r, n, 440.0, 0.05f); // under the threshold: untouched
    auto quiet = makeConfigured(EffectType::Compressor, {-18.0f, 4.0f, 1.0f, 50.0f, 0.0f, 0.0f, 100.0f});
    runEffect(*quiet, l, r);
    c.check(std::fabs(peakFrom(l, n / 2) - 0.05f) < 0.002f, "Effects: compressor leaves quiet sound alone", number("%.4f", peakFrom(l, n / 2)));
    sine(l, r, n * 2, 440.0, 0.5f);
    auto makeup = makeConfigured(EffectType::Compressor, {0.0f, 1.0f, 1.0f, 50.0f, 0.0f, 6.0f, 100.0f}); // ratio 1: only makeup
    runEffect(*makeup, l, r);
    c.check(std::fabs(peakFrom(l, n) - 0.5f * 1.9953f) < 0.02f, "Effects: compressor makeup gain", number("%.3f", peakFrom(l, n)));
    // The quick decibel conversions the gain computers use, against the exact ones over their range
    {
        double worstDb = 0.0, worstGain = 0.0;
        for (float a = 1e-5f; a < 8.0f; a *= 1.013f)
            worstDb = std::max(worstDb, static_cast<double>(std::fabs(audio::fastDecibels(a) - audio::toDecibels(a))));
        for (float db = -100.0f; db < 24.0f; db += 0.037f) {
            const float exact = audio::fromDecibels(db);
            worstGain = std::max(worstGain, static_cast<double>(std::fabs(audio::fastDbToLinear(db) - exact) / exact));
        }
        c.check(worstDb < 1e-3 && worstGain < 1e-4, "Effects: the quick decibel conversions are within a thousandth of a dB",
                number("%.5f dB", static_cast<float>(worstDb)) + number(", %.6f of the gain", static_cast<float>(worstGain)));
    }
    // Its detector's high-pass: a loud 60 Hz tone is compressed with it off (20 Hz) and all but ignored
    // with it at 1 kHz, where the tone is far below the cutoff
    auto lowTone = [&](float hpf) {
        sine(l, r, n * 2, 60.0, 0.5f);
        auto c2 = makeConfigured(EffectType::Compressor, {-18.0f, 4.0f, 1.0f, 50.0f, 0.0f, 0.0f, 100.0f, hpf});
        runEffect(*c2, l, r);
        return toDb(peakFrom(l, n));
    };
    const float hpfOff = lowTone(20.0f), hpfOn = lowTone(1000.0f);
    c.check(hpfOff < -12.0f && hpfOn > -8.0f, "Effects: the compressor's detect HPF keeps lows from driving it",
            number("%.1f dB off, ", hpfOff) + number("%.1f dB at 1 kHz", hpfOn));
    // The gate's: a 60 Hz tone over its threshold opens it, unless its detector filters the tone out
    auto gated = [&](float hpf) {
        sine(l, r, n, 60.0, 0.3f);
        auto g = makeConfigured(EffectType::Gate, {-30.0f, 1.0f, 20.0f, -80.0f, hpf});
        runEffect(*g, l, r);
        return peakFrom(l, n / 2);
    };
    c.check(gated(20.0f) > 0.25f && gated(1000.0f) < 0.01f, "Effects: the gate's detect HPF keeps lows from opening it",
            number("%.3f open, ", gated(20.0f)) + number("%.4f filtered", gated(1000.0f)));

    // Limiter: nothing gets past the ceiling, even a loud sine
    sine(l, r, n, 200.0, 2.0f);
    auto limiter = makeConfigured(EffectType::Limiter, {0.0f, -6.0f, 80.0f});
    runEffect(*limiter, l, r);
    const float ceiling = 0.5012f;
    c.check(peakFrom(l, 0) <= ceiling + 1e-4f && peakFrom(l, n / 2) > ceiling * 0.9f, "Effects: limiter holds the ceiling",
            number("peak %.4f", peakFrom(l, 0)) + number(" of %.4f", ceiling));
    sine(l, r, n, 200.0, 0.1f);
    auto passing = makeConfigured(EffectType::Limiter, {0.0f, -0.3f, 80.0f});
    runEffect(*passing, l, r);
    c.check(std::fabs(peakFrom(l, n / 2) - 0.1f) < 0.002f, "Effects: limiter passes quiet sound", number("%.4f", peakFrom(l, n / 2)));

    // Gate: shuts a quiet tone, lets a loud one through
    sine(l, r, n, 300.0, 0.003f);
    auto gate = makeConfigured(EffectType::Gate, {-40.0f, 1.0f, 50.0f, -80.0f});
    runEffect(*gate, l, r);
    c.check(peakFrom(l, n / 2) < 0.0005f, "Effects: gate closes under its threshold", number("%.5f", peakFrom(l, n / 2)));
    sine(l, r, n, 300.0, 0.3f);
    auto open = makeConfigured(EffectType::Gate, {-40.0f, 1.0f, 50.0f, -80.0f});
    runEffect(*open, l, r);
    c.check(std::fabs(peakFrom(l, n / 2) - 0.3f) < 0.005f, "Effects: gate opens over its threshold", number("%.4f", peakFrom(l, n / 2)));

    // Saturation: a hard drive keeps the peak bounded; a mix of zero is dry
    sine(l, r, n, 200.0, 0.8f);
    auto hard = makeConfigured(EffectType::Saturation, {1.0f, 20.0f, 100.0f, 0.0f});
    runEffect(*hard, l, r);
    c.check(peakFrom(l, n / 2) <= 1.0001f && peakFrom(l, n / 2) > 0.99f, "Effects: hard clipping", number("%.4f", peakFrom(l, n / 2)));
    sine(l, r, n, 200.0, 0.8f);
    auto soft = makeConfigured(EffectType::Saturation, {0.0f, 12.0f, 0.0f, 0.0f});
    runEffect(*soft, l, r);
    c.check(std::fabs(peakFrom(l, n / 2) - 0.8f) < 0.005f, "Effects: saturation mix of zero is dry", number("%.4f", peakFrom(l, n / 2)));

    // Delay: an impulse comes back after the delay time, at the feedback's level
    l.assign(n, 0.0f);
    r.assign(n, 0.0f);
    l[0] = r[0] = 1.0f;
    auto delay = makeConfigured(EffectType::Delay, {100.0f, 50.0f, 18000.0f, 0.0f, 100.0f});
    runEffect(*delay, l, r);
    size_t echo = 0;
    float echoValue = 0.0f;
    for (size_t i = 100; i < 6000; ++i) {
        if (l[i] > echoValue) {
            echoValue = l[i];
            echo = i;
        }
    }
    c.check(std::abs(static_cast<int>(echo) - 4800) <= 2 && echoValue > 0.6f, "Effects: delay echo at its time",
            number("at %.0f samples", static_cast<double>(echo)) + number(", level %.2f", echoValue));

    // Reverb: a click leaves a tail; with the mix at zero there is none
    l.assign(n, 0.0f);
    r.assign(n, 0.0f);
    l[0] = r[0] = 1.0f;
    auto reverb = makeConfigured(EffectType::Reverb, {50.0f, 50.0f, 100.0f, 50.0f});
    runEffect(*reverb, l, r);
    float tail = 0.0f;
    for (size_t i = 2000; i < 12000; ++i)
        tail = std::max(tail, std::fabs(l[i]));
    c.check(tail > 0.001f, "Effects: reverb tail", number("%.4f", tail));
    l.assign(n, 0.0f);
    r.assign(n, 0.0f);
    l[0] = r[0] = 1.0f;
    auto dryVerb = makeConfigured(EffectType::Reverb, {50.0f, 50.0f, 100.0f, 0.0f});
    runEffect(*dryVerb, l, r);
    float after = 0.0f;
    for (size_t i = 10; i < n; ++i)
        after = std::max(after, std::fabs(l[i]));
    c.check(after < 1e-6f, "Effects: reverb with no mix is dry", number("%.7f", after));

    // Bypass passes the dry sound
    sine(l, r, n, 500.0, 0.5f);
    auto bypassed = makeConfigured(EffectType::Gain, {-60.0f});
    bypassed->setBypass(true);
    runEffect(*bypassed, l, r);
    c.check(std::fabs(peakFrom(l, n / 2) - 0.5f) < 0.002f, "Effects: a bypassed effect passes the sound", number("%.4f", peakFrom(l, n / 2)));

    // Settings: defaults, text, the knob curve, keys
    const audio::DeviceSetting defaults = audio::effectSetting(EffectType::Compressor);
    c.check(defaults.kind == audio::DeviceKind::Effect && defaults.id == "compressor" && defaults.value(0, 0) == -18.0 &&
                defaults.value(1, 0) == 4.0 && !defaults.bypass,
            "Effects: a setting starts at the defaults", "");
    auto param = [](EffectType type, uint32_t id) { return audio::effectParam(audio::effectInfo(type).params[id], id); };
    c.check(audio::formatParam(param(EffectType::Compressor, 1), 4.0) == "4.0:1" &&
                audio::formatParam(param(EffectType::Equalizer, 2), 1500.0) == "1.50 kHz" &&
                audio::formatParam(param(EffectType::Filter, 0), 1.0) == "High-pass",
            "Effects: parameter text", "");
    const audio::DeviceParam hz = param(EffectType::Equalizer, 2);
    c.check(std::fabs(audio::paramFromUnit(hz, audio::paramToUnit(hz, 1234.0)) - 1234.0) < 0.5 && audio::paramToUnit(hz, hz.min) == 0.0,
            "Effects: knob curve round trips", "");
    c.check(audio::makeEffect(EffectType::Reverb)->params().size() == 4 && audio::makeEffect(EffectType::Reverb)->matches(audio::effectSetting(EffectType::Reverb)) &&
                !audio::makeEffect(EffectType::Reverb)->matches(audio::effectSetting(EffectType::Delay)),
            "Effects: a built-in effect describes itself as a device", "");
    audio::EffectType found = EffectType::Gain;
    c.check(audio::effectFromKey("compressor", found) && found == EffectType::Compressor && !audio::effectFromKey("nope", found),
            "Effects: types by key", "");

    // A chain: effects in order, one taken out, freed once the audio thread has moved on
    audio::DeviceChain chain;
    chain.prepare(kRate, 4096);
    std::vector<audio::DeviceSetting> settings{audio::effectSetting(EffectType::Gain), audio::effectSetting(EffectType::Reverb)};
    settings[0].setValue(0, -6.0);
    settings[1].setValue(3, 0.0); // no reverb mix
    chain.apply(settings);
    sine(l, r, 4096, 1000.0, 0.5f);
    chain.process(l.data(), r.data(), 4096);
    c.check(chain.size() == 2 && std::fabs(peakFrom(l, 2048) - 0.25f) < 0.02f, "Effects: a chain runs in order", number("%.3f", peakFrom(l, 2048)));
    settings.erase(settings.begin());
    chain.apply(settings);
    for (int i = 0; i < 3; ++i) {
        sine(l, r, 4096, 1000.0, 0.5f);
        chain.process(l.data(), r.data(), 4096);
    }
    chain.collect();
    c.check(chain.size() == 1 && std::fabs(peakFrom(l, 2048) - 0.5f) < 0.02f, "Effects: removing an effect", number("%.3f", peakFrom(l, 2048)));
    settings.clear();
    chain.apply(settings);
    sine(l, r, 512, 1000.0, 0.5f);
    chain.process(l.data(), r.data(), 512);
    c.check(chain.size() == 0 && std::fabs(peakFrom(l, 0) - 0.5f) < 1e-5f, "Effects: an empty chain passes the sound", "");

    // On a track the chain sits before the gain and pan; on the master bus, before the master gain
    {
        env::Environment environment;
        auto &system = environment.add<audio::AudioSystem>();
        system.masterGain().set(1.0f);
        auto group = std::make_shared<audio::SoloGroup>();
        auto tone = std::make_unique<audio::ToneGenerator>();
        tone->frequency.set(1000.0f);
        tone->gain.set(0.5f);
        tone->enabled = true;
        auto &track = system.add<audio::TrackProcessor>("Track", group, std::move(tone));
        track.pan.set(0.0f);
        std::vector<audio::DeviceSetting> chainSettings{audio::effectSetting(EffectType::Gain)};
        chainSettings[0].setValue(0, -12.0);
        track.chain.apply(chainSettings);
        float p = 0.0f;
        int crossings = 0;
        measure(render(system, 1.0), 0.5, p, crossings);
        // The centre pan is -3 dB (x0.7071) on a 0.5 tone, then the effect's -12 dB
        c.check(std::fabs(p - 0.5f * 0.7071f * 0.2512f) < 0.01f, "Effects: on a track, before its gain", number("%.4f", p));
        system.masterEffects().apply(chainSettings);
        measure(render(system, 0.5), 0.25, p, crossings);
        c.check(std::fabs(p - 0.5f * 0.7071f * 0.2512f * 0.2512f) < 0.006f, "Effects: on the master bus", number("%.4f", p));
    }
}

}

int runAudioSelfTest()
{
    std::printf("Audio self-test: offline at %.0f Hz, no device\n\n", kRate);
    Checker c;

    // A system that is never run, so no device opens
    env::Environment environment;
    auto &system = environment.add<audio::AudioSystem>();
    system.masterGain().set(1.0f);

    // A 1 kHz tone at half scale: level and pitch, once the fade-in has settled
    auto &tone = system.add<audio::ToneGenerator>();
    tone.frequency.set(1000.0f);
    tone.gain.set(0.5f);
    tone.enabled = true;
    float peak = 0.0f;
    int crossings = 0;
    measure(render(system, 1.0), 0.5, peak, crossings);
    c.check(std::fabs(peak - 0.5f) < 0.01f, "Tone level is 0.5", "peak " + number("%.4f", peak));
    c.check(std::abs(crossings - 1000) <= 2, "Tone is 1000 Hz", number("%.0f zero crossings in 0.5 s", crossings));

    // Disabled, it fades out to silence
    tone.enabled = false;
    measure(render(system, 0.5), 0.25, peak, crossings);
    c.check(peak < 1e-4f, "Disabled tone fades to silence", "peak " + number("%.6f", peak));

    // Master gain applies (and glides): 0.25 of the tone's 0.5
    tone.enabled = true;
    system.masterGain().set(0.25f);
    measure(render(system, 1.0), 0.5, peak, crossings);
    c.check(std::fabs(peak - 0.125f) < 0.005f, "Master gain scales the output", "peak " + number("%.4f", peak));
    tone.enabled = false;
    render(system, 0.2);
    system.masterGain().set(1.0f);

    // The transport stands still while stopped, moves while playing, and locates
    audio::Transport &transport = system.transport();
    const uint64_t before = transport.position();
    render(system, 0.5);
    c.check(transport.position() == before, "Stopped transport doesn't move", number("position %.0f", static_cast<double>(transport.position())));
    transport.play();
    render(system, 1.0);
    c.check(transport.position() == before + 48000, "Playing transport advances 1 s", number("position %.0f", static_cast<double>(transport.position())));
    transport.locate(96000);
    render(system, 10.0 / kRate);
    c.check(transport.position() == 96010, "Locate moves the position", number("position %.0f", static_cast<double>(transport.position())));
    // Looping in beats [4.25, 4.625) at 120 BPM, samples [102000, 111000): 14990 frames to its end,
    // then three laps of 9000 and 6010 more
    transport.setLoop(4.25, 4.625);
    transport.setLooping(true);
    render(system, 1.0);
    c.check(transport.position() == 108010, "The loop jumps back to its start on the sample",
            number("position %.0f", static_cast<double>(transport.position())));
    transport.setLooping(false);
    render(system, 10.0 / kRate);
    c.check(transport.position() == 108020, "Without looping it plays on", number("position %.0f", static_cast<double>(transport.position())));

    // The metronome at 120 BPM clicks on beats 0, 0.5, 1 and 1.5 s: four clicks in two seconds
    auto &metronome = system.add<audio::Metronome>();
    metronome.enabled = true;
    transport.stop();
    transport.locate(0);
    transport.setTempo(120.0);
    render(system, 0.01); // applies the locate while stopped
    transport.play();
    measure(render(system, 2.0), 0.0, peak, crossings);
    c.check(metronome.clicks() == 4, "Metronome clicks once per beat", number("%.0f clicks", static_cast<double>(metronome.clicks())));
    c.check(peak > 0.1f, "Metronome is audible", "peak " + number("%.3f", peak));

    // Removing a processor takes it out of the graph and deletes it
    const size_t count = system.processorCount();
    system.remove(tone);
    c.check(system.processorCount() == count - 1, "Removed processor is deleted", number("%.0f left", static_cast<double>(system.processorCount())));

    // Tracks: gain, pan law, mute and solo, with the transport and metronome quiet
    metronome.enabled = false;
    transport.stop();
    render(system, 0.1);
    auto group = std::make_shared<audio::SoloGroup>();
    auto &a = toneTrack(system, "A", group);
    float left = 0.0f;
    float right = 0.0f;
    // Centred at unity gain, each side is -3 dB (cos 45 degrees)
    const float centre = std::cos(0.7853982f);
    stereoPeaks(render(system, 0.5), 0.3, left, right);
    c.check(std::fabs(left - centre) < 0.01f && std::fabs(right - centre) < 0.01f, "Centred track is -3 dB each side",
            number("L %.4f", left) + number("  R %.4f", right));
    a.takePeak(0);
    render(system, 0.1);
    const float metered = a.takePeak(0);
    c.check(std::fabs(metered - centre) < 0.01f, "Track meter reads its output", "peak " + number("%.4f", metered));

    a.gain.set(0.5f);
    stereoPeaks(render(system, 0.5), 0.3, left, right);
    c.check(std::fabs(left - 0.5f * centre) < 0.01f && std::fabs(right - 0.5f * centre) < 0.01f, "Track gain scales it",
            number("L %.4f", left) + number("  R %.4f", right));
    a.gain.set(1.0f);

    a.pan.set(-1.0f);
    stereoPeaks(render(system, 0.5), 0.3, left, right);
    c.check(std::fabs(left - 1.0f) < 0.01f && right < 1e-3f, "Hard left pan", number("L %.4f", left) + number("  R %.4f", right));
    a.pan.set(1.0f);
    stereoPeaks(render(system, 0.5), 0.3, left, right);
    c.check(left < 1e-3f && std::fabs(right - 1.0f) < 0.01f, "Hard right pan", number("L %.4f", left) + number("  R %.4f", right));
    a.pan.set(0.5f);
    stereoPeaks(render(system, 0.5), 0.3, left, right);
    const float power = left * left + right * right;
    c.check(std::fabs(power - 1.0f) < 0.02f && right > left, "Pan keeps constant power",
            number("L %.4f", left) + number("  R %.4f", right) + number("  L2+R2 %.4f", power));
    a.pan.set(0.0f);

    a.mute = true;
    stereoPeaks(render(system, 0.5), 0.3, left, right);
    c.check(std::max(left, right) < 1e-4f, "Muted track is silent", number("peak %.6f", std::max(left, right)));
    a.mute = false;

    // Two tracks hard left and right: soloing B silences A, and unsoloing brings A back
    auto &b = toneTrack(system, "B", group);
    a.pan.set(-1.0f);
    b.pan.set(1.0f);
    b.setSolo(true);
    stereoPeaks(render(system, 0.5), 0.3, left, right);
    c.check(left < 1e-4f && std::fabs(right - 1.0f) < 0.01f, "Solo silences the other tracks",
            number("L %.4f", left) + number("  R %.4f", right));
    a.setSolo(true);
    stereoPeaks(render(system, 0.5), 0.3, left, right);
    c.check(std::fabs(left - 1.0f) < 0.01f && std::fabs(right - 1.0f) < 0.01f, "Two soloed tracks are both heard",
            number("L %.4f", left) + number("  R %.4f", right));
    a.setSolo(false);
    b.setSolo(false);
    stereoPeaks(render(system, 0.5), 0.3, left, right);
    c.check(group->soloed() == 0 && std::fabs(left - 1.0f) < 0.01f && std::fabs(right - 1.0f) < 0.01f,
            "Unsolo restores every track", number("L %.4f", left) + number("  R %.4f", right));
    b.setSolo(true);
    b.mute = true;
    stereoPeaks(render(system, 0.5), 0.3, left, right);
    c.check(std::max(left, right) < 1e-4f, "Mute beats solo", number("peak %.6f", std::max(left, right)));
    // Removing a soloed track releases its solo
    system.remove(b);
    c.check(group->soloed() == 0, "Removed soloed track unsolos", number("%.0f soloed", group->soloed()));
    stereoPeaks(render(system, 0.5), 0.3, left, right);
    c.check(std::fabs(left - 1.0f) < 0.01f, "Other tracks are heard again", number("L %.4f", left));
    system.remove(a);

    // Note names
    const bool names = audio::keyFromName("C4") == 60 && audio::keyFromName("A#1") == 34 &&
                       audio::keyFromName("Bb3") == 58 && audio::keyFromName("E2") == 40 &&
                       audio::keyFromName("H2") == -1 && audio::keyName(61) == "C#4";
    c.check(names, "Note names map to MIDI keys", "C4 60, A#1 34, Bb3 58, E2 40");

    // The sampler, with one zone: a 440 Hz sine at half scale recorded as A4 (key 69)
    auto sine = std::make_shared<audio::SampleSet>();
    sine->name = "Test sine";
    sine->releaseSeconds = 0.1f;
    {
        audio::SampleZone zone;
        zone.rootKey = 69;
        zone.sampleRate = kRate;
        zone.frames = static_cast<uint32_t>(kRate * 4.0);
        std::vector<float> samples(zone.frames);
        for (uint32_t i = 0; i < zone.frames; ++i)
            samples[i] = 0.5f * static_cast<float>(std::sin(2.0 * 3.14159265358979 * 440.0 * i / kRate));
        zone.setSamples(samples);
        sine->zones.push_back(std::move(zone));
    }
    auto &player = system.add<audio::NotePlayer>(std::make_unique<audio::Sampler>());
    auto &sampler = static_cast<audio::Sampler &>(player.instrument());
    sampler.setSampleSet(sine);
    transport.stop();

    player.audition(69, 1.0f);
    std::vector<float> out = render(system, 0.5);
    measure(out, 0.1, peak, crossings);
    c.check(std::abs(crossings - 352) <= 3, "Sampler plays its root key at pitch", number("%.0f crossings in 0.4 s (440 Hz)", crossings));
    c.check(std::fabs(peak - 0.5f) < 0.01f, "Sampler plays at the sample's level", "peak " + number("%.4f", peak));
    player.release(69);
    measure(render(system, 0.3), 0.2, peak, crossings);
    c.check(peak < 1e-3f, "Note off fades out over the release", "peak " + number("%.6f", peak));

    player.audition(81, 1.0f);
    measure(render(system, 0.5), 0.1, peak, crossings);
    c.check(std::abs(crossings - 704) <= 3, "An octave up plays at twice the pitch", number("%.0f crossings in 0.4 s (880 Hz)", crossings));
    player.release(81);
    render(system, 0.3);

    // The tempo map. A new map keeps the playhead on its beat: 2 beats at 120 BPM is sample 48000,
    // at 60 BPM sample 96000.
    {
        const bool clicking = metronome.enabled.load();
        metronome.enabled = false;
        transport.stop();
        transport.setTempo(120.0);
        transport.locate(48000);
        render(system, 0.01);
        transport.setTempo(60.0);
        render(system, 0.01);
        c.check(transport.position() == 96000, "Tempo map: the playhead keeps its beat when the tempo changes",
                number("position %.0f", static_cast<double>(transport.position())));

        // A note on beat 5 after 4 beats at 120 and then 60 BPM starts at 2 + 1 = 3 s
        audio::TempoMap change;
        change.setPoints({{0.0, 120.0, false}, {4.0, 60.0, false}});
        transport.setTiming(change);
        transport.locate(0);
        auto sequence = std::make_unique<audio::NoteSequence>();
        sequence->notes.push_back({5.0, 6.0, 69, 1.0f});
        player.setSequence(std::move(sequence));
        render(system, 0.01);
        transport.play();
        const std::vector<float> played = render(system, 3.5);
        size_t onset = 0;
        while (onset < played.size() / 2 && std::fabs(played[onset * 2]) < 0.01f)
            ++onset;
        c.check(onset >= 144000 && onset < 144000 + 48, "Tempo map: a note after a tempo change starts on its sample",
                number("at %.0f", static_cast<double>(onset)));
        transport.stop();
        player.setSequence(std::make_unique<audio::NoteSequence>());
        render(system, 0.3);

        // In 6/8 at 120 BPM the click is on every eighth: 8 in two seconds
        metronome.enabled = true;
        transport.setTiming(audio::TempoMap(120.0, 6, 8));
        transport.locate(0);
        render(system, 0.01);
        const uint64_t clicks = metronome.clicks();
        transport.play();
        render(system, 2.0);
        c.check(metronome.clicks() - clicks == 8, "Tempo map: the metronome clicks the signature's unit",
                number("%.0f clicks", static_cast<double>(metronome.clicks() - clicks)));
        transport.stop();
        transport.setTempo(120.0);
        transport.locate(0);
        render(system, 0.3);
        metronome.enabled = clicking;
    }

    // Velocity layers and loops: a soft zone (0.2) and a hard one (0.8) on one key, each a
    // quarter of a second of 480 Hz looped (100-frame periods), so they sound past their end
    {
        auto layered = std::make_shared<audio::SampleSet>();
        layered->releaseSeconds = 0.05f;
        for (int layer = 0; layer < 2; ++layer) {
            audio::SampleZone zone;
            zone.rootKey = 69;
            zone.sampleRate = kRate;
            zone.frames = 12000;
            zone.lowVelocity = layer ? 64 : 1;
            zone.highVelocity = layer ? 127 : 63;
            zone.loopStart = 2000;
            zone.loopEnd = 11900;
            std::vector<float> samples(zone.frames);
            for (uint32_t i = 0; i < zone.frames; ++i)
                samples[i] = (layer ? 0.8f : 0.2f) * static_cast<float>(std::sin(2.0 * 3.14159265358979 * 480.0 * i / kRate));
            zone.setSamples(samples);
            layered->zones.push_back(std::move(zone));
        }
        auto &lp = system.add<audio::NotePlayer>(std::make_unique<audio::Sampler>());
        static_cast<audio::Sampler &>(lp.instrument()).setSampleSet(layered);
        lp.audition(69, 0.3f); // MIDI velocity 38
        measure(render(system, 1.0), 0.6, peak, crossings);
        c.check(std::fabs(peak - 0.06f) < 0.003f, "A soft note plays the soft layer", "peak " + number("%.4f", peak) + " (0.3 x 0.2)");
        c.check(std::abs(crossings - 384) <= 3, "A loop keeps the note sounding past its end", number("%.0f crossings in 0.4 s", crossings));
        lp.release(69);
        render(system, 0.3);
        lp.audition(69, 1.0f);
        measure(render(system, 0.5), 0.3, peak, crossings);
        c.check(std::fabs(peak - 0.8f) < 0.01f, "A hard note plays the hard layer", "peak " + number("%.4f", peak));
        lp.release(69);
        render(system, 0.3);
        system.remove(lp);
    }

    // The browser's sound preview: plays a file at its own rate, ends, and stops with a fade
    {
        std::vector<float> tone(12000);
        for (size_t i = 0; i < tone.size(); ++i)
            tone[i] = 0.5f * static_cast<float>(std::sin(2.0 * 3.14159265358979 * 240.0 * i / 24000.0));
        auto data = audio::makeAudioData("preview", 24000.0, 1, tone); // half a second at 24 kHz
        auto &preview = system.add<audio::SoundPreview>();
        preview.gain.set(1.0f);
        preview.play(data);
        out = render(system, 0.3);
        measure(out, 0.05, peak, crossings);
        c.check(std::fabs(peak - 0.5f) < 0.01f && std::abs(crossings - 120) <= 2 && preview.playing(),
                "A preview plays at the file's own rate", "peak " + number("%.3f", peak) + number(", %.0f crossings", crossings));
        measure(render(system, 0.4), 0.25, peak, crossings);
        c.check(peak < 1e-6f && !preview.playing(), "A preview ends with its file", "peak " + number("%.6f", peak));
        preview.play(data);
        render(system, 0.1);
        preview.stop();
        measure(render(system, 0.1), 0.02, peak, crossings);
        c.check(peak < 1e-6f && !preview.playing(), "Stopping a preview fades it out", "peak " + number("%.6f", peak));
        preview.collect();
        system.remove(preview);
    }

    // A note on beat 2 (at 120 BPM, 0.5 s in) for one beat
    {
        auto sequence = std::make_unique<audio::NoteSequence>();
        sequence->notes.push_back({1.0, 2.0, 69, 1.0f});
        player.setSequence(std::move(sequence));
    }
    transport.setTempo(120.0);
    transport.locate(0);
    transport.play();
    out = render(system, 1.5);
    size_t onset = out.size();
    for (size_t i = 0; i < out.size() / 2; ++i) {
        if (std::fabs(out[i * 2]) > 1e-3f) {
            onset = i;
            break;
        }
    }
    c.check(onset >= 24000 && onset <= 24002, "Sequenced note starts on its beat", number("first sound at frame %.0f (beat 2 is 24000)", static_cast<double>(onset)));
    measure(out, 1.25, peak, crossings);
    c.check(peak < 1e-3f, "Sequenced note ends on its beat", "peak after its release " + number("%.6f", peak));
    transport.stop();
    render(system, 0.1);
    player.collect();

    // Controller curves: the sequence's sustain pedal (down until beat 3, 1.5 s) holds the same
    // note past its end, and the bend at the top raises it two semitones (493.9 Hz)
    {
        auto sequence = std::make_unique<audio::NoteSequence>();
        sequence->notes.push_back({1.0, 2.0, 69, 1.0f});
        audio::ControlCurve pedal;
        pedal.controller = 64;
        pedal.stepped = true;
        pedal.points = {{0.0, 1.0f}, {3.0, 0.0f}};
        audio::ControlCurve bend;
        bend.controller = audio::Instrument::kPitchBend;
        bend.points = {{0.0, 1.0f}};
        sequence->controls = {pedal, bend};
        player.setSequence(std::move(sequence));
    }
    transport.locate(0);
    transport.play();
    out = render(system, 1.4);
    measure(out, 1.0, peak, crossings);
    c.check(peak > 0.1f, "The sustain pedal holds a note past its end", "peak " + number("%.3f", peak));
    c.check(std::abs(crossings - 395) <= 4, "The pitch bend bends the note", number("%.0f crossings in 0.4 s (493.9 Hz)", crossings));
    measure(render(system, 0.6), 0.4, peak, crossings);
    c.check(peak < 1e-3f, "Lifting the pedal lets the note go", "peak " + number("%.6f", peak));
    transport.stop();
    render(system, 0.1);
    player.setSequence(std::make_unique<audio::NoteSequence>());
    render(system, 0.01);
    player.collect();

    // The guitar from the sample library (the default folder, as the app finds it), if it has been imported
    const std::string guitar = daw::Settings::defaultLibraryPath() + "/Instruments/Spanish Classical Guitar";
    std::string error;
    if (std::shared_ptr<audio::SampleSet> set = audio::loadInstrumentFolder(guitar, error)) {
        c.check(set->zones.size() >= 40, "Guitar library instrument loads", number("%.0f zones", static_cast<double>(set->zones.size())) +
                                                                                number(", %.1f MB", set->bytes() / 1e6));
        sampler.setSampleSet(set);
        render(system, 0.01);
        player.audition(52, 0.8f); // E3
        measure(render(system, 0.5), 0.0, peak, crossings);
        c.check(peak > 0.01f && peak < 1.0f, "Guitar plays E3", "peak " + number("%.3f", peak));
        player.release(52);
        render(system, 0.1);
    } else {
        std::printf("SKIP  Guitar library instrument               %s\n", error.empty() ? "not found" : error.c_str());
    }
    system.remove(player);
    render(system, 0.01);

    // MIDI: a hub first in the graph, then a player that listens to it
    auto &hub = system.add<audio::MidiHub>();
    auto &keys = system.add<audio::NotePlayer>(std::make_unique<audio::Sampler>());
    static_cast<audio::Sampler &>(keys.instrument()).setSampleSet(sine);
    keys.setMidiSource(&hub);
    keys.listen = true;
    render(system, 0.01);
    hub.send(audio::MidiMessage::noteOn(69, 127));
    measure(render(system, 0.5), 0.1, peak, crossings);
    c.check(std::abs(crossings - 352) <= 3 && std::fabs(peak - 0.5f) < 0.01f, "A MIDI note plays a listening track",
            number("peak %.3f", peak) + number(", %.0f crossings", crossings));
    audio::StampedMidi stamped;
    const bool gotStamp = hub.takeStamped(stamped);
    c.check(gotStamp && stamped.message.isNoteOn() && stamped.message.data1 == 69 && !stamped.playing,
            "MIDI is stamped for recording", gotStamp ? number("key %.0f", stamped.message.data1) : "nothing");
    while (hub.takeStamped(stamped)) {
    }
    hub.send({0xB0, 64, 127, 0}); // sustain down
    hub.send(audio::MidiMessage::noteOff(69));
    measure(render(system, 0.3), 0.1, peak, crossings);
    c.check(peak > 0.4f, "The sustain pedal holds a released key", "peak " + number("%.3f", peak));
    hub.send({0xB0, 64, 0, 0}); // sustain up
    measure(render(system, 0.4), 0.3, peak, crossings);
    c.check(peak < 1e-3f, "Lifting the pedal releases it", "peak " + number("%.6f", peak));
    hub.send(audio::MidiMessage::noteOn(69, 100));
    render(system, 0.1);
    keys.listen = false;
    measure(render(system, 0.4), 0.3, peak, crossings);
    c.check(peak < 1e-3f, "A track that stops listening lets go", "peak " + number("%.6f", peak));
    keys.listen = true;
    hub.send(audio::MidiMessage::noteOn(70, 100));
    keys.listen = false;
    render(system, 0.05);
    system.remove(keys);
    system.remove(hub);
    render(system, 0.01);

    // Audio clips: a ramp (each sample's value is its time in seconds, so the output says where
    // in the file it read) at 44.1 kHz, played on a 48 kHz timeline
    std::vector<float> ramp(44100 * 2);
    for (size_t i = 0; i < ramp.size(); ++i)
        ramp[i] = static_cast<float>(i) / 44100.0f;
    auto rampData = audio::makeAudioData("ramp", 44100.0, 1, ramp);
    auto &clips = system.add<audio::AudioClipPlayer>();
    auto play = [&](std::vector<audio::ClipRegion> regions, double seconds) {
        auto sequence = std::make_unique<audio::ClipSequence>();
        sequence->regions = std::move(regions);
        sequence->sort();
        sequence->crossfade(audio::TempoMap(120.0));
        clips.setSequence(std::move(sequence));
        transport.stop();
        transport.locate(0);
        transport.play();
        std::vector<float> o = render(system, seconds);
        transport.stop();
        clips.collect();
        return o;
    };
    {
        audio::ClipRegion r;
        r.start = 1.0; // 0.5 s at 120 BPM
        r.end = 3.0;
        r.data = rampData;
        r.offset = 0.25; // trimmed: starts a quarter second into the file
        out = play({r}, 2.0);
        size_t first = out.size();
        for (size_t i = 0; i < out.size() / 2; ++i) {
            if (out[i * 2] != 0.0f) {
                first = i;
                break;
            }
        }
        const float at = out[(24000 + 4800) * 2]; // 0.1 s into the clip
        c.check(first >= 24000 && first <= 24001, "An audio clip starts on its beat",
                number("first sound at frame %.0f", static_cast<double>(first)));
        c.check(std::fabs(at - 0.35f) < 1e-3f && std::fabs(out[(24000 + 4800) * 2 + 1] - at) < 1e-6f,
                "It reads from its trimmed offset, resampled", number("%.4f s into the file (0.35)", at));
        const float after = out[(72000 + 10) * 2];
        c.check(after == 0.0f, "It ends at its end", number("%.4f after", after));
    }
    {
        audio::ClipRegion r;
        r.start = 0.0;
        r.end = 2.0;
        r.data = rampData;
        r.offset = 0.5;
        r.fadeIn = 0.5;
        r.gain = 0.5f;
        out = play({r}, 0.6);
        const float at = out[12000 * 2]; // 0.25 s: halfway through the fade in
        const float expected = 0.75f * 0.5f * std::sin(0.5f * 1.5707963f);
        c.check(std::fabs(at - expected) < 2e-3f, "Fades are equal-power and gain scales",
                number("%.4f", at) + number(" (expected %.4f)", expected));
    }
    {
        // Two clips of a constant half-scale signal overlapping by one beat crossfade
        auto dc = audio::makeAudioData("dc", 48000.0, 1, std::vector<float>(48000 * 3, 0.5f));
        audio::ClipRegion a, b;
        a.data = b.data = dc;
        a.start = 0.0;
        a.end = 3.0;
        b.start = 2.0;
        b.end = 5.0;
        out = play({a, b}, 2.5);
        const float before = out[24000 * 2];   // beat 1: a alone
        const float middle = out[60000 * 2];   // beat 2.5: half way across
        const float later = out[96000 * 2 + 0]; // beat 4: b alone
        c.check(std::fabs(before - 0.5f) < 1e-3f && std::fabs(later - 0.5f) < 1e-3f &&
                    std::fabs(middle - 0.5f * 1.41421356f) < 5e-3f,
                "Overlapping clips crossfade", number("%.3f", before) + number(" / %.3f", middle) + number(" / %.3f", later));
    }

    // Streamed clips: a long recording plays from its own float WAV through the MemoryManager
    // (only its preload and overview in RAM) and sounds exactly as it does from memory, started
    // mid-file and crossfaded, with the disk served between blocks
    {
        namespace fs = std::filesystem;
        const fs::path folder = fs::temp_directory_path() / "vybn-clip-stream-selftest";
        std::error_code ec;
        fs::remove_all(folder, ec);
        fs::create_directories(folder, ec);
        const std::string wav = (folder / "Take.wav").u8string();
        const size_t frames = 44100 * 6;
        std::vector<float> wave(frames * 2);
        for (size_t f = 0; f < frames; ++f) {
            const float t = static_cast<float>(f) / 44100.0f;
            wave[f * 2] = 0.4f * std::sin(2.0f * 3.14159265f * 220.0f * t) + t / 12.0f;
            wave[f * 2 + 1] = 0.4f * std::sin(2.0f * 3.14159265f * 330.0f * t) - t / 12.0f;
        }
        std::string error;
        const bool wrote = audio::writeWavFile(wav, wave.data(), frames, 2, 44100.0, error);

        env::Environment streamEnv;
        auto &streamSystem = streamEnv.add<audio::AudioSystem>();
        streamSystem.masterGain().set(1.0f);
        audio::MemoryConfig config;
        config.preloadFrames = 8192;
        config.chunkFrames = 4096; // float chunks are half that: 2048 frames
        config.budgetBytes = 32ull * 4096 * 4;
        config.cacheFolder = (folder / "cache").u8string();
        auto &memory = streamEnv.add<audio::MemoryManager>(config);
        auto whole = audio::makeAudioData(wav, 44100.0, 2, wave);
        auto streamed = wrote ? audio::makeAudioData(wav, 44100.0, 2, wave, &memory) : nullptr;
        const bool isStreamed = streamed && streamed->streamed() && streamed->samples.empty();
        c.check(isStreamed && streamed->bytes() == (8192 * 2 + whole->peaks.size()) * sizeof(float) &&
                    streamed->peaks == whole->peaks && whole->bytes() > 20 * streamed->bytes(),
                "Streaming: a long recording plays from its WAV",
                streamed ? number("%.0f KB in RAM", streamed->bytes() / 1024.0) + number(" of %.0f KB", whole->bytes() / 1024.0)
                         : error);

        auto &player = streamSystem.add<audio::AudioClipPlayer>();
        audio::Transport &streamTransport = streamSystem.transport();
        auto playBoth = [&](std::shared_ptr<const audio::AudioData> data, bool serve) {
            // a: from 2 s into the file (well past the preload); b: from 0.5 s, overlapping a's
            // last beat, so they crossfade
            audio::ClipRegion a, b;
            a.data = b.data = data;
            a.start = 0.0;
            a.end = 4.0;
            a.offset = 2.0;
            b.start = 3.0;
            b.end = 8.0;
            b.offset = 0.5;
            b.gain = 0.8f;
            b.fadeOut = 0.25;
            auto sequence = std::make_unique<audio::ClipSequence>();
            sequence->regions = {a, b};
            sequence->sort();
            sequence->crossfade(audio::TempoMap(120.0));
            player.setSequence(std::move(sequence));
            streamTransport.stop();
            streamTransport.locate(0);
            std::vector<float> block(512), o;
            streamSystem.renderOffline(block.data(), 256, kRate); // stopped: the clips ask ahead
            if (serve)
                memory.poll();
            streamTransport.play();
            for (int i = 0; i < 4 * 48000 / 256 + 10; ++i) {
                streamSystem.renderOffline(block.data(), 256, kRate);
                o.insert(o.end(), block.begin(), block.end());
                if (serve)
                    memory.poll();
            }
            streamTransport.stop();
            streamSystem.renderOffline(block.data(), 16, kRate);
            player.collect();
            return o;
        };
        std::vector<float> reference = playBoth(whole, false), played;
        if (isStreamed)
            played = playBoth(streamed, true);
        float worst = 1.0f;
        float loudest = 0.0f;
        if (played.size() == reference.size()) {
            worst = 0.0f;
            for (size_t i = 0; i < played.size(); ++i) {
                worst = std::max(worst, std::fabs(played[i] - reference[i]));
                loudest = std::max(loudest, std::fabs(reference[i]));
            }
        }
        const audio::MemoryManager::Stats stats = memory.stats();
        c.check(worst == 0.0f && loudest > 0.3f && stats.underruns == 0 && stats.reads > 20,
                "Streaming: mid-file and crossfaded, as from memory",
                number("%.0f chunks read", static_cast<double>(stats.reads)) +
                    number(", %.0f underruns", static_cast<double>(stats.underruns)));

        // An empty sequence hands the last one back
        player.setSequence(std::make_unique<audio::ClipSequence>());
        std::vector<float> block(64);
        streamSystem.renderOffline(block.data(), 16, kRate);
        player.collect();
        streamed.reset();
        memory.poll();
        c.check(memory.stats().sources == 0, "Streaming: a clip's source goes with its data",
                number("%.0f left", memory.stats().sources));
    }
    {
        std::error_code ec;
        std::filesystem::remove_all(std::filesystem::temp_directory_path() / "vybn-clip-stream-selftest", ec);
    }

    // Input: monitoring and recording, with a fake stereo input
    {
        const uint32_t frames = 4800;
        std::vector<float> input(frames * 2);
        for (uint32_t i = 0; i < frames; ++i) {
            input[i * 2] = 0.25f;
            input[i * 2 + 1] = -0.125f;
        }
        clips.monitor = true;
        clips.inputChannel = 1;
        clips.inputStereo = false;
        transport.stop();
        std::vector<float> o(frames * 2);
        system.renderOffline(o.data(), frames, kRate, input.data(), 2);
        c.check(std::fabs(o[100] - -0.125f) < 1e-4f && std::fabs(o[101] - -0.125f) < 1e-4f,
                "Monitoring passes an input channel through", number("%.4f", o[100]) + number(" / %.4f", o[101]));
        clips.monitor = false;

        auto &recorder = system.add<audio::InputRecorder>();
        recorder.arm();
        system.renderOffline(o.data(), frames, kRate, input.data(), 2); // stopped: nothing yet
        transport.locate(48000);
        transport.play();
        system.renderOffline(o.data(), frames, kRate, input.data(), 2);
        system.renderOffline(o.data(), frames, kRate, input.data(), 2);
        recorder.disarm();
        system.renderOffline(o.data(), frames, kRate, input.data(), 2);
        transport.stop();
        std::vector<float> take;
        const size_t got = recorder.drain(take);
        c.check(recorder.started() && recorder.startPosition() == 48000 && recorder.channels() == 2,
                "A take starts where the transport was", number("at %.0f", static_cast<double>(recorder.startPosition())));
        c.check(got == frames * 2 && take.size() == got * 2 && take[0] == 0.25f && take[1] == -0.125f &&
                    !recorder.capturing(),
                "Recording captures the input until disarmed", number("%.0f frames", static_cast<double>(got)));
        // Its ring goes between takes and comes back for the next
        const bool freed = recorder.releaseMemory();
        recorder.arm();
        const bool heldWhileArmed = !recorder.releaseMemory();
        transport.play();
        system.renderOffline(o.data(), frames, kRate, input.data(), 2);
        recorder.disarm();
        system.renderOffline(o.data(), frames, kRate, input.data(), 2);
        transport.stop();
        take.clear();
        const size_t again = recorder.drain(take);
        c.check(freed && heldWhileArmed && again == frames && take[0] == 0.25f && recorder.releaseMemory(),
                "The recorder's memory is freed between takes", number("%.0f frames after", static_cast<double>(again)));
        system.remove(recorder);
    }
    system.remove(clips);
    render(system, 0.01);

    // A recording written to a WAV file reads back
    {
        std::string werror;
        const std::string wav = std::string(std::getenv("TEMP") ? std::getenv("TEMP") : ".") + "/vybn-selftest.wav";
        std::vector<float> stereo(2000);
        for (size_t i = 0; i < stereo.size(); ++i)
            stereo[i] = (i % 2 ? -0.5f : 0.5f) * static_cast<float>(i) / 2000.0f;
        const bool wrote = audio::writeWavFile(wav, stereo.data(), 1000, 2, 48000.0, werror);
        auto back = wrote ? audio::loadAudioFile(wav, werror) : nullptr;
        c.check(back && back->frames == 1000 && back->channels == 2 && back->sampleRate == 48000.0 &&
                    std::fabs(back->samples[999] - stereo[999]) < 1e-6f && !back->peaks.empty(),
                "WAV files write and read back", werror);
        std::remove(wav.c_str());
    }

    // Integer WAV files: 24-bit exact to a step, 16-bit within its dither, and clipped at full scale
    for (int bits : {16, 24}) {
        std::string werror;
        const std::string wav = std::string(std::getenv("TEMP") ? std::getenv("TEMP") : ".") + "/vybn-selftest-int.wav";
        std::vector<float> stereo(2000);
        for (size_t i = 0; i < stereo.size(); ++i)
            stereo[i] = std::sin(static_cast<float>(i) * 0.01f) * 0.9f;
        stereo[10] = 1.5f;
        const bool wrote = audio::writeWavFile(wav, stereo.data(), 1000, 2, 44100.0, werror, bits);
        auto back = wrote ? audio::loadAudioFile(wav, werror) : nullptr;
        float worst = 0.0f;
        for (size_t i = 0; back && i < stereo.size() && i < back->samples.size(); ++i) {
            if (i != 10)
                worst = std::max(worst, std::fabs(back->samples[i] - stereo[i]));
        }
        const float step = bits == 16 ? 1.0f / 32768.0f : 1.0f / 8388608.0f;
        c.check(back && back->frames == 1000 && back->sampleRate == 44100.0 && worst <= step * (bits == 16 ? 2.0f : 1.0f) &&
                    back->samples[10] > 0.999f && back->samples[10] <= 1.0f,
                bits == 16 ? "16-bit WAV files write and read back" : "24-bit WAV files write and read back",
                number("off by %.2f steps ", worst / step) + werror);
        std::remove(wav.c_str());
    }

    effectChecks(c);

    // Delay compensation: a track's sound held back by exactly its compensation, across blocks,
    // and the compensations lining tracks up with the latest
    {
        // A one-sample click at the first frame it renders
        struct Click : audio::AudioProcessor {
            Click() : AudioProcessor("Click") {}
            bool done = false;
            void process(audio::AudioBlock &block) override
            {
                if (!done)
                    for (uint32_t ch = 0; ch < block.channelCount; ++ch)
                        block.channels[ch][0] += 1.0f;
                done = true;
            }
        };
        audio::TransportState state;
        auto clickAt = [&](uint32_t delay) {
            audio::TrackProcessor track("Late", nullptr, std::make_unique<Click>());
            track.prepare(48000.0, 512);
            track.compensation = delay;
            std::vector<float> l(512), r(512);
            float *channels[2] = {l.data(), r.data()};
            int found = -1;
            for (int b = 0; b < 4 && found < 0; ++b) {
                std::fill(l.begin(), l.end(), 0.0f);
                std::fill(r.begin(), r.end(), 0.0f);
                audio::AudioBlock block{channels, 2, 512, 48000.0, state};
                track.process(block);
                for (int i = 0; i < 512; ++i)
                    if (std::fabs(l[static_cast<size_t>(i)]) > 1e-9f) { // the level glides up from silence
                        found = b * 512 + i;
                        break;
                    }
            }
            return found;
        };
        const int none = clickAt(0), some = clickAt(700);
        c.check(none == 0 && some == 700, "Compensation: a track is held back exactly", std::to_string(some) + " frames");
        const std::vector<uint32_t> delays = audio::compensations({0, 256, 64, 99999});
        c.check(delays[0] == audio::TrackProcessor::kMaxCompensation && delays[1] == audio::TrackProcessor::kMaxCompensation - 256 &&
                    delays[3] == 0 && audio::compensations({0, 0})[0] == 0,
                "Compensation: tracks line up with the latest (within its limit)", "");
    }

    // The deviceless engine (exports): no device, the same output as the device path, its taps
    // adding up to the master, the offline flag per thread, and a normal-priority pool
    {
        // A tone at a frequency, gain and pan on its own track
        auto addTone = [](audio::AudioSystem &system, const std::string &name, float frequency, float gain, float pan) -> audio::TrackProcessor & {
            auto tone = std::make_unique<audio::ToneGenerator>();
            tone->frequency.set(frequency);
            tone->gain.set(1.0f);
            tone->enabled = true;
            auto &track = system.add<audio::TrackProcessor>(name, std::make_shared<audio::SoloGroup>(), std::move(tone));
            track.gain.set(gain);
            track.pan.set(pan);
            return track;
        };
        // Three tracks at different levels and pans
        auto buildGraph = [&](audio::AudioSystem &system) {
            addTone(system, "Low", 440.0f, 0.5f, -0.3f);
            addTone(system, "Mid", 1000.0f, 1.25f, 0.8f);
            addTone(system, "Quiet", 660.0f, 1.0f, 0.0f);
        };
        constexpr uint32_t kBlock = audio::AudioSystem::kMaxBlock;
        constexpr uint32_t kChannels = audio::AudioSystem::kChannels;

        env::Environment normalEnv;
        auto &normal = normalEnv.add<audio::AudioSystem>();
        normal.masterGain().set(1.0f);
        env::Environment offlineEnv;
        auto &offline = offlineEnv.add<audio::AudioSystem>(audio::Deviceless{});
        offline.masterGain().set(1.0f);
        buildGraph(normal);
        buildGraph(offline);
        const std::vector<float> expected = render(normal, 0.5);
        const std::vector<float> actual = render(offline, 0.5);
        float difference = 0.0f;
        for (size_t i = 0; i < expected.size() && i < actual.size(); ++i)
            difference = std::max(difference, std::fabs(expected[i] - actual[i]));
        c.check(expected.size() == actual.size() && difference < 1e-6f && peakFrom(actual, 0) > 0.1f,
                "Deviceless renders the same as the device path", number("max difference %.2e", difference));
        c.check(offline.devices().empty() && !offline.running() && !offline.lastError().empty(),
                "Deviceless opens no device and says why", offline.lastError());

        // Post-fader taps of every track sum to the master, with the master at unity; the muted
        // track's tap is silence. Each block is its own render, so the taps hold that block.
        env::Environment tapEnv;
        auto &taps = tapEnv.add<audio::AudioSystem>(audio::Deviceless{});
        taps.masterGain().set(1.0f);
        audio::TrackProcessor *tapped[3] = {&addTone(taps, "A", 440.0f, 0.5f, -0.3f), &addTone(taps, "B", 1000.0f, 1.25f, 0.8f),
                                            &addTone(taps, "C", 660.0f, 2.0f, 0.5f)};
        tapped[2]->mute = true;
        std::vector<float> tapLeft[3], tapRight[3];
        for (int t = 0; t < 3; ++t) {
            tapLeft[t].assign(kBlock, 0.0f);
            tapRight[t].assign(kBlock, 0.0f);
            tapped[t]->setTap(tapLeft[t].data(), tapRight[t].data(), false);
        }
        std::vector<float> mixed(kBlock * kChannels);
        float worst = 0.0f, heard = 0.0f, mutedTap = 0.0f;
        for (int block = 0; block < 8; ++block) {
            taps.renderOffline(mixed.data(), kBlock, kRate);
            for (uint32_t i = 0; i < kBlock; ++i) {
                float sumL = 0.0f, sumR = 0.0f;
                for (int t = 0; t < 3; ++t) {
                    sumL += tapLeft[t][i];
                    sumR += tapRight[t][i];
                }
                worst = std::max({worst, std::fabs(sumL - mixed[i * 2]), std::fabs(sumR - mixed[i * 2 + 1])});
                heard = std::max(heard, std::fabs(mixed[i * 2]));
                mutedTap = std::max({mutedTap, std::fabs(tapLeft[2][i]), std::fabs(tapRight[2][i])});
            }
        }
        c.check(worst < 1e-6f && heard > 0.1f, "Post-fader taps sum to the master", number("worst difference %.2e", worst));
        c.check(mutedTap == 0.0f, "A muted track's post-fader tap is silence", number("peak %.2e", mutedTap));

        // Pre-fader taps ignore the track's gain and mute; post-fader ones don't. Four tracks play
        // the same tone: X (gain 0.25, pre), Y (gain 2, muted, pre), Z (gain 0.25, post), W (gain 2,
        // muted, post).
        env::Environment preEnv;
        auto &pre = preEnv.add<audio::AudioSystem>(audio::Deviceless{});
        pre.masterGain().set(1.0f);
        audio::TrackProcessor &x = addTone(pre, "X", 440.0f, 0.25f, 0.0f);
        audio::TrackProcessor &y = addTone(pre, "Y", 440.0f, 2.0f, 0.0f);
        audio::TrackProcessor &z = addTone(pre, "Z", 440.0f, 0.25f, 0.0f);
        audio::TrackProcessor &w = addTone(pre, "W", 440.0f, 2.0f, 0.0f);
        y.mute = true;
        w.mute = true;
        std::vector<float> xL(kBlock), xR(kBlock), yL(kBlock), yR(kBlock), zL(kBlock), zR(kBlock), wL(kBlock), wR(kBlock);
        x.setTap(xL.data(), xR.data(), true);
        y.setTap(yL.data(), yR.data(), true);
        z.setTap(zL.data(), zR.data(), false);
        w.setTap(wL.data(), wR.data(), false);
        float preDifference = 0.0f, postError = 0.0f, mutedPost = 0.0f, xPeak = 0.0f;
        const float panned = 0.25f * 0.70710678f; // a quarter, centred: cos(pi/4) per channel
        for (int block = 0; block < 8; ++block) {
            std::vector<float> out(kBlock * kChannels);
            pre.renderOffline(out.data(), kBlock, kRate);
            for (uint32_t i = 0; i < kBlock; ++i) {
                preDifference = std::max({preDifference, std::fabs(xL[i] - yL[i]), std::fabs(xR[i] - yR[i])});
                xPeak = std::max(xPeak, std::fabs(xL[i]));
                mutedPost = std::max({mutedPost, std::fabs(wL[i]), std::fabs(wR[i])});
                if (block == 7)
                    postError = std::max({postError, std::fabs(zL[i] - panned * xL[i]), std::fabs(zR[i] - panned * xR[i])});
            }
        }
        c.check(preDifference < 1e-6f && xPeak > 0.5f, "Pre-fader taps ignore the gain and mute", number("difference %.2e", preDifference));
        c.check(postError < 1e-3f && mutedPost == 0.0f, "Post-fader taps follow the gain, and mute",
                number("error %.2e", postError) + number(", muted peak %.2e", mutedPost));

        // The offline flag is per thread: set here, it isn't seen on another thread
        audio::setRenderingOffline(true);
        bool elsewhere = true;
        std::thread([&elsewhere] { elsewhere = audio::renderingOffline(); }).join();
        const bool here = audio::renderingOffline();
        audio::setRenderingOffline(false);
        c.check(here && !elsewhere && !audio::renderingOffline(), "The rendering-offline flag is per thread", "");

        // A normal-priority pool of two workers, each marked offline as it starts, renders the same
        // as no pool (24 tracks, so the batches really split across the workers)
        static std::atomic<int> markedWorkers{0};
        env::RealtimePool pool(false);
        pool.onWorkerStart([] {
            audio::setRenderingOffline(true);
            if (audio::renderingOffline())
                markedWorkers.fetch_add(1);
        });
        pool.start(2);
        for (int i = 0; i < 2000 && markedWorkers.load() < 2; ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        env::Environment soloEnv;
        auto &solo = soloEnv.add<audio::AudioSystem>(audio::Deviceless{});
        env::Environment pooledEnv;
        auto &pooled = pooledEnv.add<audio::AudioSystem>(audio::Deviceless{});
        pooled.setPool(&pool);
        solo.masterGain().set(1.0f);
        pooled.masterGain().set(1.0f);
        for (int t = 0; t < 24; ++t) {
            const float frequency = 200.0f + 37.0f * static_cast<float>(t);
            const float pan = -1.0f + 2.0f * static_cast<float>(t) / 23.0f;
            const float gain = 0.3f + 0.05f * static_cast<float>(t);
            addTone(solo, "Track " + std::to_string(t), frequency, gain, pan);
            addTone(pooled, "Track " + std::to_string(t), frequency, gain, pan);
        }
        const std::vector<float> single = render(solo, 0.5);
        const std::vector<float> parallel = render(pooled, 0.5);
        float poolDifference = 0.0f;
        for (size_t i = 0; i < single.size() && i < parallel.size(); ++i)
            poolDifference = std::max(poolDifference, std::fabs(single[i] - parallel[i]));
        c.check(markedWorkers.load() == 2 && single.size() == parallel.size() && poolDifference < 1e-6f && peakFrom(parallel, 0) > 0.1f,
                "A deviceless pool of normal-priority workers renders the same",
                number("max difference %.2e", poolDifference) + ", " + std::to_string(markedWorkers.load()) + " marked workers");
    }

    // Note expression: the note player sends a note's curve from its note-on, as it changes
    {
        struct Heard {
            bool expression;
            int key;
            float value;
            uint64_t sample;
        };
        // An instrument that takes timed events and writes down what it got
        struct Listener : audio::Instrument {
            Listener() : Instrument("Listener") {}
            std::vector<Heard> heard;
            uint64_t at = 0;
            void noteOn(int key, float velocity) override { heard.push_back({false, key, velocity, at}); }
            void noteOff(int key) override { heard.push_back({false, key, 0.0f, at}); }
            void allNotesOff() override {}
            bool timedEvents() const override { return true; }
            void noteOnAt(int key, float velocity, uint32_t frame) override { heard.push_back({false, key, velocity, at + frame}); }
            void noteOffAt(int key, uint32_t frame) override { heard.push_back({false, key, 0.0f, at + frame}); }
            void noteExpressionAt(int key, int expression, float value, uint32_t frame) override
            {
                if (expression == kTuning)
                    heard.push_back({true, key, value, at + frame});
            }
            void process(audio::AudioBlock &) override {}
        };
        auto owned = std::make_unique<Listener>();
        Listener *listener = owned.get();
        audio::NotePlayer player(std::move(owned));
        constexpr double kRate = 48000.0;
        constexpr uint32_t kBlock = 512;
        player.prepare(kRate, kBlock);
        // At 120 BPM a beat is 24000 samples: a glide over half a beat, a plain note, a held tuning
        auto sequence = [](bool curves) {
            auto s = std::make_unique<audio::NoteSequence>();
            s->notes.push_back({0.1, 1.0, 60, 0.8f, 0, curves ? 1u : 0u});
            s->notes.push_back({0.1, 1.0, 64, 0.8f, 0, 0});
            s->notes.push_back({0.1, 1.0, 67, 0.8f, curves ? 1u : 0u, curves ? 1u : 0u});
            audio::ExpressionCurve glide, held;
            glide.points = {{0.0, 0.0f}, {0.5, 12.0f}};
            held.points = {{0.0, 3.0f}};
            if (curves) {
                s->curves.push_back(glide);
                s->curves.push_back(held);
            }
            return s;
        };
        player.setSequence(sequence(true));
        std::vector<float> left(kBlock), right(kBlock);
        float *channels[2] = {left.data(), right.data()};
        audio::TransportState t;
        t.playing = true;
        t.sampleRate = kRate;
        auto play = [&](int blocks) {
            for (int b = 0; b < blocks; ++b) {
                listener->at = t.position;
                audio::AudioBlock block{channels, 2, kBlock, kRate, t};
                player.process(block);
                t.position += kBlock;
            }
        };
        play(40); // to beat 0.85, past the glide's end
        std::vector<Heard> glide, plain, held;
        size_t onAt = listener->heard.size();
        for (size_t i = 0; i < listener->heard.size(); ++i) {
            const Heard &h = listener->heard[i];
            if (!h.expression && h.key == 60 && h.value > 0.0f && onAt == listener->heard.size())
                onAt = i;
            if (h.expression)
                (h.key == 60 ? glide : h.key == 64 ? plain : held).push_back(h);
        }
        const uint64_t start = 2400; // beat 0.1
        bool rising = glide.size() > 2;
        for (size_t i = 1; i < glide.size(); ++i)
            rising = rising && glide[i].value > glide[i - 1].value && glide[i].sample - glide[i - 1].sample <= 64;
        const bool afterOn = !glide.empty() && glide.front().sample == start && glide.front().value == 0.0f && onAt < listener->heard.size() &&
                             std::find_if(listener->heard.begin(), listener->heard.end(), [](const Heard &h) { return h.expression; }) >
                                 listener->heard.begin() + static_cast<std::ptrdiff_t>(onAt);
        c.check(afterOn, "Note expression: a note's curve starts at its note-on, sent just after it");
        c.check(rising && !glide.empty() && std::fabs(glide.back().value - 12.0f) < 1e-4f &&
                    glide.size() > 12000 / 64,
                "Note expression: a glide is sent at least every 64 frames while it moves, and not once it is still",
                std::to_string(glide.size()) + " values, the last " + number("%.2f", glide.empty() ? 0.0f : glide.back().value));
        c.check(plain.empty() && held.size() == 1 && held[0].value == 3.0f,
                "Note expression: nothing for a note without curves, one value for a still one",
                std::to_string(plain.size()) + " and " + std::to_string(held.size()));
        // The curves go while the notes sound: their tuning goes back to rest
        const size_t before = listener->heard.size();
        player.setSequence(sequence(false));
        play(1);
        int rests = 0;
        for (size_t i = before; i < listener->heard.size(); ++i)
            rests += listener->heard[i].expression && listener->heard[i].value == 0.0f && listener->heard[i].key != 64 ? 1 : 0;
        c.check(rests == 2, "Note expression: a curve taken away from a sounding note sends it back to rest",
                std::to_string(rests) + " rests");
    }

    c.failures += runWarpAudioChecks();
    c.failures += runAnalysisChecks();
    return c.finish("audio");
}

}
