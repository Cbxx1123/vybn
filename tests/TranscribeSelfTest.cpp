#include "TranscribeSelfTest.h"
#include "Harness.h"

#include <cmath>
#include <string>
#include <vector>

#include "audio/Transcribe.h"
#include "daw/model/Transcription.h"

namespace tests {

namespace {

constexpr double kRate = 48000.0;
constexpr double kTwoPi = 6.283185307179586;

bool near(double a, double b)
{
    return std::fabs(a - b) < 1e-9;
}

std::string text(double v)
{
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%.3g", v);
    return buffer;
}

// White noise, the same each run
struct Noise {
    uint32_t seed = 1;
    float next()
    {
        seed = seed * 1664525u + 1013904223u;
        return static_cast<float>(static_cast<double>(seed >> 8) / 16777216.0 * 2.0 - 1.0);
    }
};

}

int runTranscribeChecks()
{
    Checker c;

    // Drums: a kick (a falling low sine), a snare (a low body and dark noise), a closed hat and an open
    // one (bright noise, short and long)
    {
        std::vector<float> x(static_cast<size_t>(kRate * 2.6), 0.0f);
        Noise noise;
        auto put = [&](double at, double seconds, auto voice) {
            const auto start = static_cast<size_t>(at * kRate);
            for (size_t i = 0; i < static_cast<size_t>(seconds * kRate) && start + i < x.size(); ++i)
                x[start + i] += voice(static_cast<double>(i) / kRate);
        };
        auto kick = [](double t) { return static_cast<float>(0.8 * std::exp(-t * 18.0) * std::sin(kTwoPi * (50.0 * t + 70.0 * (1.0 - std::exp(-t * 25.0)) / 25.0))); };
        float lowpassed = 0.0f;
        auto snare = [&](double t) {
            lowpassed += (noise.next() - lowpassed) * 0.25f; // about 2.5 kHz
            return static_cast<float>(std::exp(-t * 22.0) * (0.4 * std::sin(kTwoPi * 190.0 * t) + 0.9 * lowpassed));
        };
        float previous = 0.0f;
        auto bright = [&](double decay) {
            return [&, decay](double t) {
                const float n = noise.next();
                const float high = n - previous; // a difference: only the highs
                previous = n;
                return static_cast<float>(0.5 * std::exp(-t / decay) * high);
            };
        };
        put(0.1, 0.4, kick);
        put(0.6, 0.3, snare);
        put(1.1, 0.1, bright(0.012));
        put(1.6, 0.4, kick);
        put(2.0, 0.5, bright(0.12));
        const std::vector<audio::TranscribedNote> hits = audio::transcribeDrums(x.data(), x.size(), 1, kRate);
        const int expected[] = {audio::kKickKey, audio::kSnareKey, audio::kClosedHatKey, audio::kKickKey, audio::kOpenHatKey};
        const double times[] = {0.1, 0.6, 1.1, 1.6, 2.0};
        bool ok = hits.size() == 5;
        double worst = 0.0;
        std::string keys;
        for (size_t i = 0; i < hits.size(); ++i) {
            keys += std::to_string(hits[i].key) + "@" + text(hits[i].start) + " ";
            if (i < 5) {
                ok = ok && hits[i].key == expected[i];
                worst = std::max(worst, std::fabs(hits[i].start - times[i]));
            }
        }
        c.check(ok && worst < 0.003, "Audio to MIDI: drums named and placed", keys + "worst " + text(worst * 1000.0) + " ms");
    }

    // Melody: A3, E4 and C4 half a second each, then a glide from A4 up a semitone, kept as tuning
    {
        std::vector<float> x(static_cast<size_t>(kRate * 2.4), 0.0f);
        double phase = 0.0;
        auto tone = [&](double from, double to, double startHz, double endHz) {
            const auto a = static_cast<size_t>(from * kRate), b = static_cast<size_t>(to * kRate);
            for (size_t i = a; i < b && i < x.size(); ++i) {
                const double t = static_cast<double>(i - a) / static_cast<double>(b - a);
                const double hz = startHz * std::pow(endHz / startHz, t);
                phase += kTwoPi * hz / kRate;
                const double edge = std::min(1.0, std::min(t, 1.0 - t) * (to - from) / 0.01); // 10 ms fades
                x[i] = static_cast<float>(0.5 * edge * std::sin(phase));
            }
        };
        tone(0.0, 0.5, 220.0, 220.0);
        tone(0.5, 1.0, 329.63, 329.63);
        tone(1.0, 1.5, 261.63, 261.63);
        tone(1.6, 2.3, 440.0, 466.16);
        const std::vector<audio::TranscribedNote> notes = audio::transcribeMelody(x.data(), x.size(), 1, kRate);
        std::string keys;
        for (const audio::TranscribedNote &n : notes)
            keys += std::to_string(n.key) + "@" + text(n.start) + "+" + text(n.length) + "t" + std::to_string(n.tuning.size()) + " ";
        bool ok = notes.size() == 4 && notes[0].key == 57 && notes[1].key == 64 && notes[2].key == 60 && (notes[3].key == 69 || notes[3].key == 70);
        for (size_t i = 0; ok && i < 3; ++i)
            ok = std::fabs(notes[i].start - 0.5 * static_cast<double>(i)) < 0.03 && std::fabs(notes[i].length - 0.5) < 0.06 && notes[i].tuning.empty();
        ok = ok && !notes[3].tuning.empty() && notes[3].tuning.back().second > notes[3].tuning.front().second + 0.5f;
        c.check(ok, "Audio to MIDI: a melody's keys, starts and lengths, a glide kept as tuning", keys);
    }

    // Plucks ringing over each other (the --demo audioclip take): one note each, the ring of the one before
    // not splitting the next
    {
        std::vector<float> x(static_cast<size_t>(kRate * 6.0), 0.0f);
        const double keys[] = {220.0, 261.63, 329.63, 196.0, 246.94, 293.66};
        for (int pluck = 0; pluck < 12; ++pluck) {
            const auto start = static_cast<size_t>(pluck * 0.5 * kRate);
            for (size_t i = 0; start + i < x.size() && i < static_cast<size_t>(kRate * 1.2); ++i) {
                const double t = static_cast<double>(i) / kRate;
                x[start + i] += static_cast<float>(0.35 * std::exp(-t * 4.0) * std::sin(kTwoPi * keys[pluck % 6] * t));
            }
        }
        const std::vector<audio::TranscribedNote> notes = audio::transcribeMelody(x.data(), x.size(), 1, kRate);
        std::string found;
        for (const audio::TranscribedNote &n : notes)
            found += std::to_string(n.key) + "@" + text(n.start) + "+" + text(n.length) + " ";
        bool ok = notes.size() == 12;
        const int expected[] = {57, 60, 64, 55, 59, 62};
        for (size_t i = 0; ok && i < notes.size(); ++i)
            ok = notes[i].key == expected[i % 6] && std::fabs(notes[i].start - 0.5 * static_cast<double>(i)) < 0.03;
        c.check(ok, "Audio to MIDI: ringing plucks, one note each", found);
    }

    // Harmony: C, A minor and G7, two seconds each, as sines (a bass note under the chord), on beats of 0.5 s
    {
        const double seconds = 6.0;
        std::vector<float> x(static_cast<size_t>(kRate * seconds), 0.0f);
        auto chord = [&](double from, double to, std::vector<int> keys) {
            for (size_t i = static_cast<size_t>(from * kRate); i < static_cast<size_t>(to * kRate) && i < x.size(); ++i) {
                double v = 0.0;
                for (int key : keys)
                    v += std::sin(kTwoPi * 440.0 * std::pow(2.0, (key - 69) / 12.0) * static_cast<double>(i) / kRate);
                x[i] = static_cast<float>(0.15 * v);
            }
        };
        chord(0.0, 2.0, {48, 60, 64, 67});
        chord(2.0, 4.0, {45, 57, 60, 64});
        chord(4.0, 6.0, {43, 55, 59, 62, 65});
        std::vector<double> beats;
        for (int b = 0; b <= 12; ++b)
            beats.push_back(b * 0.5);
        const std::vector<audio::TranscribedChord> chords = audio::transcribeHarmony(x.data(), x.size(), 1, kRate, beats);
        std::string names;
        for (const audio::TranscribedChord &ch : chords)
            names += ch.name + "(" + text(ch.start) + "-" + text(ch.end) + ") ";
        const bool ok = chords.size() == 3 && chords[0].name == "C" && chords[1].name == "Am" && chords[2].name == "G7" &&
                        chords[1].start == 2.0 && chords[2].end == 6.0 && chords[0].keys() == std::vector<int>{48, 60, 64, 67};
        c.check(ok, "Audio to MIDI: chords found over their beats", names);
    }

    return c.failures;
}

int runTranscriptionProjectChecks()
{
    Checker c;
    daw::Project p;
    p.timing = audio::TempoMap(120.0, 4, 4);
    daw::Clip audio("Loop", 8.0, 4.0); // two seconds at 120, from bar 3
    audio.audioFile = "loop.wav";
    audio.offset = 1.0;
    // Notes at 1.5 s and 2.5 s into the file: half a beat and two and a half beats into the clip
    std::vector<audio::TranscribedNote> found(3);
    found[0].start = 1.5;
    found[0].length = 0.5;
    found[0].key = 60;
    found[1].start = 2.5;
    found[1].length = 0.25;
    found[1].key = 62;
    found[1].tuning = {{0.0, 0.0f}, {0.25, 0.8f}};
    found[2].start = 0.5; // before the clip's start: left out
    const daw::Clip melody = daw::transcribedClip(p, audio, found, daw::Conversion::Melody);
    const bool placed = melody.start == 8.0 && melody.length == 4.0 && melody.notes.size() == 2 && near(melody.notes[0].start, 1.0) &&
                        near(melody.notes[0].length, 1.0) && near(melody.notes[1].start, 3.0) &&
                        melody.notes[1].curve(daw::Expression::Tuning) != nullptr && melody.name == "Loop melody";
    const daw::Clip drums = daw::transcribedClip(p, audio, found, daw::Conversion::Drums);
    // Warped at 60 BPM in the file: a second of the file is a beat of the song
    daw::Clip warped = audio;
    warped.warp.on = true;
    warped.warp.bpm = 60.0;
    const daw::Clip onBeats = daw::transcribedClip(p, warped, found, daw::Conversion::Melody);
    const std::vector<double> boundaries = daw::clipBeatBoundaries(p, audio);
    c.check(placed && drums.notes.size() == 2 && drums.notes[0].length == 0.25 && onBeats.notes.size() == 2 &&
                near(onBeats.notes[0].start, 0.5) && boundaries.size() == 5 && near(boundaries.front(), 1.0) && near(boundaries.back(), 3.0),
            "Audio to MIDI: converted notes land on the clip's beats, warped or not");

    // Chords over their beats
    audio::TranscribedChord am;
    am.start = 1.0;
    am.end = 2.0;
    am.root = 9;
    am.quality = audio::ChordQuality::Minor;
    const daw::Clip harmony = daw::chordClip(p, audio, {am});
    c.check(harmony.notes.size() == 4 && near(harmony.notes[0].start, 0.0) && near(harmony.notes[0].length, 2.0) && harmony.notes[0].key == 57,
            "Audio to MIDI: a chord plays over its beats");
    return c.failures;
}

}
