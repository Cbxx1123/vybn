#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace audio {

// Audio to MIDI (docs/design/audio-to-midi-plan.md): a sound's notes, worked out offline on a background
// thread from its samples (interleaved, any channels, mixed to mono). Times are seconds into the sound.

// A note found: when it starts and how long it lasts, its key and velocity (0-1), and how far its pitch
// strays from the key over it (seconds from its start, semitones; empty when it holds)
struct TranscribedNote {
    double start = 0.0;
    double length = 0.0;
    int key = 60;
    float velocity = 0.8f;
    std::vector<std::pair<double, float>> tuning;
};

// Drums: each hit at its transient, a kick when its lows lead (under 150 Hz), a hat when its highs do
// (over 6 kHz; open when it rings past 150 ms), else a snare; hits under a tenth of the loudest left
// out. General MIDI keys: 36, 38, 42, 46.
constexpr int kKickKey = 36, kSnareKey = 38, kClosedHatKey = 42, kOpenHatKey = 46;
std::vector<TranscribedNote> transcribeDrums(const float *interleaved, uint64_t frames, uint32_t channels, double sampleRate);

// The pitch every hop: YIN (the cumulative mean normalised difference, its first dip under the
// threshold, else its deepest when that is still clear, refined by a parabola), 50 Hz to 1.5 kHz; 0
// where it isn't voiced (no clear dip, or quieter than 45 dB under the loudest)
struct PitchTrack {
    double hopSeconds = 0.005;
    std::vector<float> hz;    // per hop, 0 unvoiced
    std::vector<float> level; // per hop, RMS
};
PitchTrack trackPitch(const float *interleaved, uint64_t frames, uint32_t channels, double sampleRate);

// Melody: notes where the pitch holds within half a semitone and the sound stays up (a median over the
// track first), split at jumps, at gaps and at new hits; at least 60 ms. Velocity from the level, and the
// pitch's drift kept as tuning.
std::vector<TranscribedNote> transcribeMelody(const float *interleaved, uint64_t frames, uint32_t channels, double sampleRate);

// A chord over a stretch: its root (0 is C), its quality, and its name ("Am", "G7")
enum class ChordQuality { Major, Minor, Seventh, MinorSeventh, Diminished, Suspended };
struct TranscribedChord {
    double start = 0.0;
    double end = 0.0;
    int root = 0;
    ChordQuality quality = ChordQuality::Major;
    std::string name;
    // Its notes to play: the root in octave 3, the chord's tones in octave 4
    std::vector<int> keys() const;
};
// Harmony: a chromagram (12 pitch classes, 55 Hz to 2 kHz, every 50 ms), averaged over each stretch
// between `boundaries` (seconds, rising: the beats), matched against the chord templates (the lowest
// strong class favouring its root); silent stretches left out, repeated chords joined.
std::vector<TranscribedChord> transcribeHarmony(const float *interleaved, uint64_t frames, uint32_t channels, double sampleRate,
                                                const std::vector<double> &boundaries);

}
