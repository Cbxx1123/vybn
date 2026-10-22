#include "SynthSelfTest.h"
#include "Harness.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdio>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

#include "audio/AudioProcessor.h"
#include "audio/DeviceChain.h"
#include "audio/NotePlayer.h"
#include "audio/Synth.h"
#include "audio/SynthPresets.h"
#include "audio/SynthTables.h"
#include "daw/SynthUserPresets.h"

namespace tests {

namespace {

constexpr double kRate = 48000.0;
constexpr double kPi = 3.14159265358979323846;

std::string number(const char *format, double value)
{
    char buffer[96];
    std::snprintf(buffer, sizeof buffer, format, value);
    return buffer;
}

// Non-finite samples seen by any render (checked once at the end)
int gNonFinite = 0;

// Renders a synth offline the way the audio thread does: blocks of 256 frames, playing or not
struct Player {
    audio::Synth &synth;
    uint64_t position = 0;
    double tempo = 120.0;
    bool playing = true;
    std::vector<float> left;
    std::vector<float> right;

    explicit Player(audio::Synth &s) : synth(s) {}

    // Renders `frames` more frames, appended to left and right
    void run(uint32_t frames)
    {
        uint32_t done = 0;
        while (done < frames) {
            const uint32_t n = std::min<uint32_t>(256, frames - done);
            std::vector<float> l(n, 0.0f), r(n, 0.0f);
            float *chans[2] = {l.data(), r.data()};
            audio::TransportState state;
            state.playing = playing;
            state.position = position;
            state.tempo = tempo;
            state.sampleRate = kRate;
            audio::AudioBlock block{chans, 2, n, kRate, state};
            synth.process(block);
            for (uint32_t i = 0; i < n; ++i) {
                if (!std::isfinite(l[i]) || !std::isfinite(r[i]))
                    ++gNonFinite;
                left.push_back(l[i]);
                right.push_back(r[i]);
            }
            position += n;
            done += n;
        }
    }

    void seconds(double s)
    {
        run(static_cast<uint32_t>(std::lround(s * kRate)));
    }

    size_t now() const { return left.size(); }
};

// The index of a time in seconds
size_t at(double seconds)
{
    return static_cast<size_t>(std::lround(seconds * kRate));
}

// Upward zero crossings per second over [from, to)
double frequency(const std::vector<float> &x, size_t from, size_t to)
{
    int crossings = 0;
    for (size_t i = std::max<size_t>(from, 1); i < to && i < x.size(); ++i)
        if (x[i - 1] < 0.0f && x[i] >= 0.0f)
            ++crossings;
    return crossings / ((to - from) / kRate);
}

double peak(const std::vector<float> &x, size_t from, size_t to)
{
    double p = 0.0;
    for (size_t i = from; i < to && i < x.size(); ++i)
        p = std::max(p, static_cast<double>(std::fabs(x[i])));
    return p;
}

double rms(const std::vector<float> &x, size_t from, size_t to)
{
    double sum = 0.0;
    size_t n = 0;
    for (size_t i = from; i < to && i < x.size(); ++i, ++n)
        sum += static_cast<double>(x[i]) * x[i];
    return n ? std::sqrt(sum / static_cast<double>(n)) : 0.0;
}

// The energy of the first difference over [from, to): the high frequencies of the signal
double differenceEnergy(const std::vector<float> &x, size_t from, size_t to)
{
    double sum = 0.0;
    for (size_t i = std::max<size_t>(from, 1); i < to && i < x.size(); ++i) {
        const double d = static_cast<double>(x[i]) - x[i - 1];
        sum += d * d;
    }
    return sum;
}

// A synth with a plain sine at full level and the filter wide open: pitch can be measured exactly
void clean(audio::Synth &s)
{
    using namespace audio::synth;
    s.setParam(kOscBase[0] + kOscWave, 0);
    s.setParam(kOscBase[0] + kOscLevel, 80);
    s.setParam(kFilterType, 0);
    s.setParam(kCutoff, 20000);
    s.setParam(kResonance, 0);
    s.setParam(kEnvAmount, 0);
    s.setParam(kKeyTracking, 0);
    s.setParam(kFilterVelocity, 0);
}

// Starts a note the way a NotePlayer does: the parameters are current before the key goes down
void keyDown(audio::Synth &s, int key, float velocity = 0.8f)
{
    s.beginBlock();
    s.noteOn(key, velocity);
}

void setSlot(audio::Synth &s, uint32_t slot, int source, int target, double amount)
{
    using namespace audio::synth;
    s.setParam(kModSlotBase + 3 * slot, source);
    s.setParam(kModSlotBase + 3 * slot + 1, target);
    s.setParam(kModSlotBase + 3 * slot + 2, amount);
}

// The magnitude of the largest bin from `from` to `to` of a spectrum
double largestBin(const std::vector<std::complex<double>> &z, size_t from, size_t to)
{
    double m = 0.0;
    for (size_t k = from; k <= to && k < z.size(); ++k)
        m = std::max(m, std::abs(z[k]));
    return m;
}

// The positive-frequency spectrum of one table frame
std::vector<std::complex<double>> spectrumOf(const float *table)
{
    std::vector<std::complex<double>> z(audio::synth::kTableSize);
    for (int i = 0; i < audio::synth::kTableSize; ++i)
        z[static_cast<std::size_t>(i)] = table[i];
    audio::synth::fft(z, false);
    return z;
}

}

int runSynthSelfTest()
{
    using namespace audio::synth;
    Checker c;

    // The wave bank: every frame of every wave peaks at 1 at the top level
    const WaveBank &bank = WaveBank::get();
    const char *waveNames[kWaveCount] = {"Sine", "Triangle", "Saw", "Square", "Basic table", "Harmonic table",
                                         "Vocal table", "PWM table"};
    for (int wave = 0; wave < kWaveCount; ++wave) {
        double worst = 0.0;
        for (int frame = 0; frame < bank.frames(wave); ++frame) {
            const float *t = bank.level(wave, frame, 0);
            double p = 0.0;
            for (int i = 0; i < kTableSize; ++i)
                p = std::max(p, static_cast<double>(std::fabs(t[i])));
            worst = std::max(worst, std::fabs(p - 1.0));
        }
        c.check(worst < 0.01, std::string(waveNames[wave]) + " frames peak at 1", number("(worst error %.4f)", worst));
    }

    // The saw's mip levels hold only their harmonics: level 5 stops at 32, level 10 at 1
    const std::vector<std::complex<double>> saw5 = spectrumOf(bank.level(kSaw, 0, 5));
    const std::vector<std::complex<double>> saw10 = spectrumOf(bank.level(kSaw, 0, 10));
    const double fundamental = std::abs(saw5[1]);
    const double aboveLevel5 = largestBin(saw5, 33, 1023);
    const double aboveLevel10 = largestBin(saw10, 2, 1023);
    c.check(std::abs(saw5[32]) > 0.01 * fundamental && aboveLevel5 < 1e-3 * fundamental,
            "Saw level 5 has its harmonics to 32 and no more",
            number("(above 32: %.2e of the fundamental)", aboveLevel5 / fundamental));
    c.check(aboveLevel10 < 1e-3 * fundamental, "Saw level 10 is a sine",
            number("(above 1: %.2e of the fundamental)", aboveLevel10 / fundamental));

    // Basic table at position 0 is a sine
    double sineError = 0.0;
    const float *basic0 = bank.level(kBasicTable, 0, 0);
    for (int i = 0; i < kTableSize; ++i)
        sineError = std::max(sineError, std::fabs(static_cast<double>(basic0[i]) - std::sin(2.0 * kPi * i / kTableSize)));
    c.check(sineError < 0.01, "Basic table at position 0 is a sine", number("(worst error %.4f)", sineError));

    // A note with the Init values: sound on both channels
    {
        audio::Synth s;
        s.prepare(kRate, 4096);
        Player p(s);
        keyDown(s, 69);
        p.seconds(0.8);
        const double l = rms(p.left, at(0.4), at(0.8));
        const double r = rms(p.right, at(0.4), at(0.8));
        c.check(l > 0.01 && r > 0.01, "A4 with the Init values sounds on both channels",
                number("(left rms %.3f)", l));
    }

    // Pitch: A4 is 440 Hz, and octave, semitone and fine move it by the right ratio
    struct PitchCase {
        const char *name;
        int key;
        double octave, semitones, fine, expected;
    };
    const PitchCase cases[] = {
        {"A4 plays at 440 Hz", 69, 0, 0, 0, 440.0},
        {"An octave up plays at 880 Hz", 69, 1, 0, 0, 880.0},
        {"Semitones +7 play at 659.3 Hz", 69, 0, 7, 0, 659.26},
        {"Fine +50 cents plays at 453.3 Hz", 69, 0, 0, 50, 453.27},
    };
    for (const PitchCase &pc : cases) {
        audio::Synth s;
        s.prepare(kRate, 4096);
        clean(s);
        s.setParam(kOscBase[0] + kOscOctave, pc.octave);
        s.setParam(kOscBase[0] + kOscSemitones, pc.semitones);
        s.setParam(kOscBase[0] + kOscFine, pc.fine);
        Player p(s);
        keyDown(s, pc.key);
        p.seconds(0.8);
        const double f = frequency(p.left, at(0.4), at(0.8));
        c.check(std::fabs(f - pc.expected) < 0.01 * pc.expected, pc.name,
                number("(%.1f Hz)", f));
    }

    // The bend: full up with a range of two semitones raises a note two semitones
    {
        audio::Synth s;
        s.prepare(kRate, 4096);
        clean(s);
        s.setParam(kBendRange, 2);
        Player p(s);
        keyDown(s, 69);
        p.seconds(0.4);
        s.controlChange(audio::Instrument::kPitchBend, 1.0f);
        p.seconds(0.5);
        const double f = frequency(p.left, at(0.5), at(0.9));
        c.check(std::fabs(f - 493.88) < 0.01 * 493.88, "Pitch bend 1.0 with range 2 raises two semitones",
                number("(%.1f Hz)", f));
    }

    // The amp envelope: a 100 ms attack, the sustain level held, released to -60 dB in its time
    {
        audio::Synth s;
        s.prepare(kRate, 4096);
        clean(s);
        s.setParam(kAmpAttack, 100);
        s.setParam(kAmpSustain, 80);
        Player p(s);
        keyDown(s, 69);
        p.seconds(1.0);
        const double halfway = peak(p.left, at(0.045), at(0.055));
        const double attackEnd = peak(p.left, at(0.095), at(0.105));
        const double sustain = peak(p.left, at(0.9), at(1.0));
        c.check(attackEnd > 0.0 && halfway / attackEnd > 0.35 && halfway / attackEnd < 0.65,
                "Amp attack rises over its time", number("(half way at %.2f of the peak)", halfway / attackEnd));
        c.check(attackEnd > 0.0 && sustain / attackEnd > 0.75 && sustain / attackEnd < 0.85,
                "Amp holds its sustain level", number("(sustain at %.2f of the peak)", sustain / attackEnd));
        s.noteOff(69);
        p.seconds(0.8);
        const double tail = peak(p.left, at(1.0 + 0.3), at(1.0 + 0.32));
        c.check(sustain > 0.0 && tail < 0.001 * sustain, "Amp releases below -60 dB within 1.2 release times",
                number("(%.1f dB)", 20.0 * std::log10(std::max(tail, 1e-12) / sustain)));
        c.check(s.activeVoices() == 0 && peak(p.left, at(1.5), at(1.6)) == 0.0 && peak(p.right, at(1.5), at(1.6)) == 0.0,
                "A released voice is freed, then silent");
    }

    // The filter: a low-pass 24 removes the highs of a saw, a high-pass the lows of a sine, and
    // the resonance raises the level at the cutoff
    {
        double high[2] = {0.0, 0.0};
        const double cutoffs[2] = {500.0, 20000.0};
        for (int i = 0; i < 2; ++i) {
            audio::Synth s;
            s.prepare(kRate, 4096);
            s.setParam(kFilterType, 1);
            s.setParam(kCutoff, cutoffs[i]);
            s.setParam(kEnvAmount, 0);
            s.setParam(kKeyTracking, 0);
            s.setParam(kFilterVelocity, 0);
            s.setParam(kResonance, 0);
            Player p(s);
            keyDown(s, 57);
            p.seconds(0.7);
            high[i] = differenceEnergy(p.left, at(0.3), at(0.6));
        }
        c.check(high[0] < 0.1 * high[1], "Low-pass 24 at 500 Hz removes a saw's highs",
                number("(%.3f of the open filter)", high[0] / high[1]));

        audio::Synth s;
        s.prepare(kRate, 4096);
        clean(s);
        s.setParam(kFilterType, 2);
        s.setParam(kCutoff, 2000);
        Player hp(s);
        keyDown(s, 45);
        hp.seconds(0.6);
        audio::Synth open;
        open.prepare(kRate, 4096);
        clean(open);
        Player lp(open);
        keyDown(open, 45);
        lp.seconds(0.6);
        const double ratio = rms(hp.left, at(0.3), at(0.6)) / rms(lp.left, at(0.3), at(0.6));
        c.check(ratio < 0.05, "High-pass 12 at 2 kHz removes a 110 Hz sine", number("(%.4f of it)", ratio));

        double resonant = 0.0, flat = 0.0;
        for (int i = 0; i < 2; ++i) {
            audio::Synth r;
            r.prepare(kRate, 4096);
            clean(r);
            r.setParam(kFilterType, 0);
            r.setParam(kCutoff, 440);
            r.setParam(kResonance, i == 0 ? 0 : 90);
            Player p(r);
            keyDown(r, 69);
            p.seconds(0.6);
            (i == 0 ? flat : resonant) = rms(p.left, at(0.3), at(0.6));
        }
        c.check(resonant > 2.0 * flat, "Resonance raises the level at the cutoff",
                number("(%.1f times)", resonant / flat));
    }

    // Unison: seven copies spread over the stereo field, still at the note's pitch
    {
        audio::Synth s;
        s.prepare(kRate, 4096);
        clean(s);
        s.setParam(kOscBase[0] + kOscUnison, 7);
        s.setParam(kOscBase[0] + kOscDetune, 20);
        s.setParam(kOscBase[0] + kOscSpread, 80);
        Player p(s);
        keyDown(s, 69);
        p.seconds(0.9);
        double difference = 0.0;
        for (size_t i = at(0.3); i < at(0.8); ++i)
            difference += std::pow(static_cast<double>(p.left[i]) - p.right[i], 2.0);
        const double width = std::sqrt(difference / static_cast<double>(at(0.8) - at(0.3)));
        const double levelL = rms(p.left, at(0.3), at(0.8));
        c.check(width > 0.05 * levelL, "Unison 7 is wider than mono", number("(side %.2f of mid)", width / levelL));
        const double f = frequency(p.left, at(0.3), at(0.8));
        c.check(std::fabs(f - 440.0) < 0.02 * 440.0, "Unison 7 stays at the note's pitch", number("(%.1f Hz)", f));
    }

    // Poly: eight notes sound; a ninth steals one of them
    {
        audio::Synth s;
        s.prepare(kRate, 4096);
        Player p(s);
        for (int key = 60; key < 68; ++key)
            keyDown(s, key);
        p.run(256);
        const int eight = s.activeVoices();
        keyDown(s, 68);
        p.run(256);
        c.check(eight == 8 && s.activeVoices() == 8, "Poly 8 sounds eight notes and a ninth steals one",
                number("(%.0f voices)", static_cast<double>(s.activeVoices())));
    }

    // Mono: a second key replaces the first, and releasing it returns to the first
    {
        audio::Synth s;
        s.prepare(kRate, 4096);
        clean(s);
        s.setParam(kVoiceMode, 1);
        Player p(s);
        keyDown(s, 60);
        p.seconds(0.3);
        const size_t secondStart = p.now();
        keyDown(s, 64);
        p.seconds(0.3);
        const double second = frequency(p.left, secondStart + at(0.05), secondStart + at(0.3));
        p.run(256);
        const bool single = s.activeVoices() == 1;
        const size_t releaseStart = p.now();
        s.noteOff(64);
        p.seconds(0.3);
        const double back = frequency(p.left, releaseStart + at(0.05), releaseStart + at(0.3));
        c.check(single && std::fabs(second - 329.63) < 0.02 * 329.63, "Mono: a second key replaces the first",
                number("(%.1f Hz)", second));
        c.check(std::fabs(back - 261.63) < 0.02 * 261.63, "Mono: releasing the second returns to the first",
                number("(%.1f Hz)", back));
    }

    // Glide: in Mono the pitch slides from the last note, most of the way there after the glide time
    {
        audio::Synth s;
        s.prepare(kRate, 4096);
        clean(s);
        s.setParam(kVoiceMode, 1);
        s.setParam(kGlide, 200);
        Player p(s);
        keyDown(s, 60);
        p.seconds(0.5);
        const size_t start = p.now();
        keyDown(s, 72);
        p.seconds(0.2);
        const double f = frequency(p.left, start + at(0.03), start + at(0.07));
        c.check(f > 330.0 && f < 480.0, "Glide 200 ms: the pitch is between the notes 50 ms in",
                number("(%.1f Hz)", f));
    }

    // LFO 1 on the pitch: the period wobbles; the LFO's rate is its own without sync
    {
        audio::Synth s;
        s.prepare(kRate, 4096);
        clean(s);
        setSlot(s, 0, 1, 1, 10.0); // LFO 1 to pitch, a tenth of its +-24 semitones: 2.4
        Player p(s);
        keyDown(s, 69);
        p.seconds(1.0);
        p.run(256);
        double shortest = 1e9, longest = 0.0;
        size_t last = 0;
        bool haveLast = false;
        for (size_t i = at(0.3) + 1; i < at(1.0); ++i) {
            if (p.left[i - 1] < 0.0f && p.left[i] >= 0.0f) {
                if (haveLast) {
                    const double period = static_cast<double>(i - last);
                    shortest = std::min(shortest, period);
                    longest = std::max(longest, period);
                }
                last = i;
                haveLast = true;
            }
        }
        c.check(longest / shortest > 1.1, "LFO 1 to pitch makes the pitch wobble",
                number("(periods differ by %.2fx)", longest / shortest));
        c.check(std::fabs(s.lfoRateHz(0) - 4.0) < 1e-6, "LFO 1 runs at its own rate without sync",
                number("(%.2f Hz)", s.lfoRateHz(0)));
    }

    // LFO sync: the rate follows the transport's tempo (a quarter note at 120 is 2 Hz)
    {
        audio::Synth s;
        s.prepare(kRate, 4096);
        s.setParam(kLfoBase[0] + kLfoSync, 1);
        s.setParam(kLfoBase[0] + kLfoSyncRate, 4);
        Player p(s);
        p.tempo = 120.0;
        p.run(256);
        const double at120 = s.lfoRateHz(0);
        p.tempo = 60.0;
        p.run(256);
        const double at60 = s.lfoRateHz(0);
        c.check(std::fabs(at120 - 2.0) < 1e-6, "LFO sync at 120 BPM gives a quarter note's rate",
                number("(%.2f Hz)", at120));
        c.check(std::fabs(at60 - 1.0) < 1e-6, "LFO sync follows the tempo", number("(%.2f Hz)", at60));
    }

    // The mod wheel as a source: full wheel bends the pitch up by the slot's 2.4 semitones
    {
        audio::Synth s;
        s.prepare(kRate, 4096);
        clean(s);
        setSlot(s, 0, 7, 1, 10.0);
        s.controlChange(1, 1.0f);
        Player p(s);
        keyDown(s, 69);
        p.seconds(0.8);
        const double f = frequency(p.left, at(0.4), at(0.8));
        const double expected = 440.0 * std::pow(2.0, 2.4 / 12.0);
        c.check(std::fabs(f - expected) < 0.02 * expected, "The mod wheel modulates the pitch",
                number("(%.1f Hz)", f));
    }

    // All notes off: the notes release, and after the release there is no sound at all
    {
        audio::Synth s;
        s.prepare(kRate, 4096);
        Player p(s);
        keyDown(s, 60);
        p.seconds(0.2);
        s.allNotesOff();
        p.seconds(1.0);
        c.check(s.activeVoices() == 0 && peak(p.left, at(1.0), at(1.2)) == 0.0 && peak(p.right, at(1.0), at(1.2)) == 0.0,
                "All notes off silences within the release");
    }

    // Sixteen voices of unison 7 saws stay within a sane level and finite
    {
        audio::Synth s;
        s.prepare(kRate, 4096);
        s.setParam(kVoices, 16);
        s.setParam(kOscBase[0] + kOscUnison, 7);
        s.setParam(kOscBase[1] + kOscWave, 2);
        s.setParam(kOscBase[1] + kOscUnison, 7);
        s.setParam(kOscBase[1] + kOscLevel, 80);
        Player p(s);
        for (int key = 48; key < 64; ++key)
            keyDown(s, key, 1.0f);
        p.seconds(1.0);
        const double top = std::max(peak(p.left, 0, p.now()), peak(p.right, 0, p.now()));
        c.check(top < 4.0 && s.activeVoices() == 16, "16 voices of unison 7 stay under a peak of 4",
                number("(peak %.2f)", top));
    }

    // The time a busy render takes: 8 voices of unison 7 with both oscillators, two seconds
    {
        audio::Synth s;
        s.prepare(kRate, 256);
        s.setParam(kOscBase[0] + kOscUnison, 7);
        s.setParam(kOscBase[1] + kOscWave, 2);
        s.setParam(kOscBase[1] + kOscUnison, 7);
        s.setParam(kOscBase[1] + kOscLevel, 80);
        std::vector<float> l(256), r(256);
        const auto start = std::chrono::steady_clock::now();
        for (int key = 48; key < 56; ++key)
            keyDown(s, key);
        audio::TransportState state;
        state.sampleRate = kRate;
        for (uint64_t frame = 0; frame < 96000; frame += 256) {
            std::fill(l.begin(), l.end(), 0.0f);
            std::fill(r.begin(), r.end(), 0.0f);
            float *chans[2] = {l.data(), r.data()};
            state.position = frame;
            state.playing = true;
            audio::AudioBlock block{chans, 2, 256, kRate, state};
            s.process(block);
        }
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        std::printf("INFO synth: %.0f ms for 2 s of 8 voices x 7 unison\n", ms);
        c.check(ms < 2000.0, "Two seconds of 8 voices x 7 unison render in under 2 s in real time",
                number("(%.0f ms)", ms));
    }

    // The device: its setting has every parameter, and it lists them
    {
        const audio::DeviceSetting setting = audio::SynthDevice::setting();
        const std::size_t listed = synthParams().size();
        c.check(setting.values.size() == listed && listed > 0, "The synth's setting holds every parameter",
                number("(%.0f parameters)", static_cast<double>(listed)));
    }

    // A track's chain makes the synth from its setting, and a note through the track's player sounds
    {
        std::unique_ptr<audio::InstrumentDevice> made = audio::makeInstrumentDevice(audio::SynthDevice::setting(), nullptr);
        c.check(made && made->kind() == audio::DeviceKind::Synth, "The chain makes a synth from its setting");
        audio::DeviceChain chain;
        chain.prepare(kRate, 4096);
        chain.apply(audio::SynthDevice::setting(), {});
        auto instrument = std::make_unique<audio::ChainInstrument>();
        instrument->setChain(&chain);
        audio::NotePlayer player(std::move(instrument));
        player.prepare(kRate, 4096);
        auto sequence = std::make_unique<audio::NoteSequence>();
        sequence->notes.push_back({1.0, 2.0, 69, 1.0f}); // a beat in at 120 BPM: half a second
        player.setSequence(std::move(sequence));
        std::vector<float> left, right;
        uint64_t position = 0;
        while (position < 48000) {
            const uint32_t n = 256;
            std::vector<float> l(n, 0.0f), r(n, 0.0f);
            float *chans[2] = {l.data(), r.data()};
            audio::TransportState state;
            state.playing = true;
            state.position = position;
            state.tempo = 120.0;
            state.sampleRate = kRate;
            audio::AudioBlock block{chans, 2, n, kRate, state};
            player.process(block);
            left.insert(left.end(), l.begin(), l.end());
            right.insert(right.end(), r.begin(), r.end());
            position += n;
        }
        const double l = rms(left, at(0.75), at(1.0));
        const double r = rms(right, at(0.75), at(1.0));
        c.check(l > 0.01 && r > 0.01, "A note through the chain's synth sounds", number("(left rms %.3f)", l));
    }

    // The factory presets: 24 of them with unique names, and every value a known parameter in range
    {
        const std::vector<audio::SynthPreset> &presets = audio::synthPresets();
        c.check(presets.size() == 24, "There are 24 factory presets, Init included",
                number("(%.0f presets)", static_cast<double>(presets.size())));
        bool unique = true;
        for (size_t i = 0; i < presets.size(); ++i)
            for (size_t j = i + 1; j < presets.size(); ++j)
                unique = unique && presets[i].name != presets[j].name;
        c.check(unique, "Preset names are unique");

        const std::vector<audio::DeviceParam> params = audio::synth::synthParams();
        bool known = true, inRange = true;
        std::string bad;
        for (const audio::SynthPreset &preset : presets) {
            for (const auto &v : preset.values) {
                const auto it = std::find_if(params.begin(), params.end(), [&](const audio::DeviceParam &p) { return p.id == v.first; });
                if (it == params.end()) {
                    known = false;
                    bad = preset.name + " " + std::to_string(v.first);
                } else if (!(v.second >= it->min && v.second <= it->max)) {
                    inRange = false;
                    bad = preset.name + " " + it->name;
                }
            }
        }
        c.check(known, "Every preset value is a known parameter", bad);
        c.check(inRange, "Every preset value is within its parameter's range", bad);
    }

    // Each preset plays a C major chord for 1.5 s, then lets go: it sounds, stays finite and under 2
    for (const audio::SynthPreset &preset : audio::synthPresets()) {
        audio::SynthDevice device(audio::SynthDevice::setting(preset.name));
        device.prepare(kRate, 256);
        const int keys[] = {48, 52, 55, 60};
        double top = 0.0;
        bool finite = true;
        uint64_t position = 0;
        const uint64_t held = 72000, tail = 24000; // 1.5 s held, then 0.5 s to ring out
        for (uint64_t frame = 0; frame < held + tail; frame += 256) {
            if (frame == held)
                for (int key : keys)
                    device.noteOff(key);
            std::vector<float> l(256, 0.0f), r(256, 0.0f);
            float *chans[2] = {l.data(), r.data()};
            audio::TransportState state;
            state.playing = true;
            state.position = position;
            state.tempo = 120.0;
            state.sampleRate = kRate;
            device.beginBlock();
            if (frame == 0)
                for (int key : keys)
                    device.noteOn(key, 0.8f);
            audio::AudioBlock block{chans, 2, 256, kRate, state};
            device.process(block);
            for (size_t i = 0; i < 256; ++i) {
                if (!std::isfinite(l[i]) || !std::isfinite(r[i]))
                    finite = false;
                top = std::max(top, static_cast<double>(std::max(std::fabs(l[i]), std::fabs(r[i]))));
            }
            position += 256;
        }
        c.check(finite, preset.name + ": no NaN or infinity");
        c.check(top > 0.001 && top < 2.0, preset.name + ": a C major chord sounds, peak under 2",
                number("(peak %.3f)", top));
    }

    // Names: a preset's setting is named after it and differs from Init; an unknown name is Init
    {
        const audio::DeviceSetting reese = audio::SynthDevice::setting("Reese");
        const audio::DeviceSetting init = audio::SynthDevice::setting();
        c.check(reese.name == "Reese" && reese != init, "A preset's setting is named after it and differs from Init");
        c.check(audio::SynthDevice::setting("No such preset").name == "Init" &&
                    audio::SynthDevice::setting("No such preset") == init,
                "An unknown preset name gives Init");
    }

    // The wave as the panel draws it: a sine peaks at its middle, a square has both signs
    {
        std::vector<float> sine(64), square(64);
        audio::synth::previewCycle(audio::synth::kSine, 0.5f, sine.data(), 64);
        audio::synth::previewCycle(audio::synth::kSquare, 0.5f, square.data(), 64);
        const float top = *std::max_element(sine.begin(), sine.end());
        c.check(top > 0.95f && top < 1.05f, "A sine's cycle peaks at 1", number("(%.3f)", top));
        int high = 0, low = 0;
        for (float v : square) {
            high += v > 0.5f ? 1 : 0;
            low += v < -0.5f ? 1 : 0;
        }
        c.check(high > 16 && low > 16, "A square's cycle is high and low for about half each", number("(%.0f high)", high));
    }

    // User presets: a round trip through a folder of their own (never the user's), with values
    // clamped and unknown ids dropped, and names a file can't have refused
    {
        namespace fs = std::filesystem;
        const fs::path folder = fs::temp_directory_path() / "vibin-synth-preset-test";
        std::error_code ec;
        fs::remove_all(folder, ec);
        audio::SynthPreset preset;
        preset.name = "Round trip";
        preset.category = "Bass";
        preset.values = {{audio::synth::kCutoff, 1234.5}, {audio::synth::kOscWave, 3}, {audio::synth::kResonance, 250.0},
                         {500, 7.0}};
        std::string error;
        const bool saved = daw::saveUserSynthPreset(preset, error, folder.string());
        const std::vector<audio::SynthPreset> loaded = daw::loadUserSynthPresets(error, folder.string());
        c.check(saved && loaded.size() == 1, "A user preset is saved and read back", error);
        if (loaded.size() == 1) {
            const audio::SynthPreset &back = loaded.front();
            auto valueOf = [&back](uint32_t id) { return plugin::valueOf(back.values, id, -1.0); };
            c.check(back.name == "Round trip" && back.category == "User", "It reads back with its name, in the User category");
            c.check(valueOf(audio::synth::kCutoff) == 1234.5 && valueOf(audio::synth::kOscWave) == 3.0,
                    "Its values read back exactly", number("(cutoff %.1f)", valueOf(audio::synth::kCutoff)));
            c.check(valueOf(audio::synth::kResonance) == 100.0, "A value above its range is clamped");
            c.check(valueOf(500) == -1.0, "An unknown id is dropped");
        }
        audio::SynthPreset bad = preset;
        bad.name = "a/b";
        std::string refused;
        c.check(!daw::saveUserSynthPreset(bad, refused, folder.string()) && !refused.empty(), "A name with a slash is refused");
        bad.name = "";
        c.check(!daw::saveUserSynthPreset(bad, refused, folder.string()) && !refused.empty(), "An empty name is refused");
        std::string none;
        c.check(daw::loadUserSynthPresets(none, (folder / "missing").string()).empty(), "A folder that doesn't exist has no presets");
        fs::remove_all(folder, ec);
    }

    c.check(gNonFinite == 0, "No NaN or infinity in any output", number("(%.0f bad samples)", gNonFinite));

    return c.finish("synth");
}

}
