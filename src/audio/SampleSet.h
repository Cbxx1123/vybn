#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace audio {

class MemoryManager;
class StreamSource;

// One recorded note of a multisampled instrument and the keys it covers. Played at
// `rootKey` it sounds as recorded; other keys in its range pitch-shift it.
struct SampleZone {
    int lowKey = 0;
    int highKey = 127;
    int rootKey = 60;
    int lowVelocity = 1; // the velocities (1-127) it plays for: one layer of several
    int highVelocity = 127;
    float tuneCents = 0.0f;     // pitch offset (the SFZ's tune and transpose)
    float keytrackCents = 100.0f; // pitch change per key away from rootKey (0: every key the same)
    // Frames looped while the note sounds, when loopEnd > loopStart (loop_mode loop_continuous
    // or loop_sustain)
    uint32_t loopStart = 0;
    uint32_t loopEnd = 0;
    double sampleRate = 44100.0;
    uint32_t channels = 1;   // 1 or 2
    uint32_t frames = 0;
    // The audio: interleaved 16-bit (full scale 32768), shared by every zone that plays the same
    // file. 16 bits is half the memory of floats and as good as playback needs.
    // When the zone streams, pcm holds only its first frames (the preload) and `stream` reads the
    // rest from disk as voices reach it; otherwise pcm holds every frame and stream is null.
    std::shared_ptr<const std::vector<int16_t>> pcm;
    std::shared_ptr<const StreamSource> stream;
    uint32_t residentFrames() const { return pcm && channels ? static_cast<uint32_t>(pcm->size() / channels) : 0; }
    float gain = 1.0f;       // from the SFZ's volume (dB), linear
    std::string file;        // where it was loaded from, for messages

    // Fills pcm (and frames) from float samples (-1 to 1, interleaved in `channels`), for tests and
    // generated sounds
    void setSamples(const std::vector<float> &interleaved);
    static constexpr float kScale = 1.0f / 32768.0f; // pcm to float
};

// A multisampled instrument loaded into memory: the data a Sampler plays. Immutable once
// loaded, so several samplers can share one.
struct SampleSet {
    std::string name;
    std::vector<SampleZone> zones;
    float releaseSeconds = 0.3f; // fade after note off (the SFZ's ampeg_release)

    // The zone for `key` at `velocity` (1-127): the first that covers both, else the nearest by
    // key among those covering the velocity, else the nearest by key; nullptr when there are none
    const SampleZone *zoneFor(int key, int velocity = 100) const;
    // Every zone covering `key` at `velocity` (layers that sound together), at most `max`, into
    // `out`; when none covers it, the one zoneFor picks. Returns how many.
    int zonesFor(int key, int velocity, const SampleZone **out, int max) const;
    // Memory the audio takes in RAM (each shared file counted once; streamed zones count their
    // preloads), and the audio streamed from disk on top of that
    uint64_t bytes() const;
    uint64_t streamedBytes() const;
};

// MIDI key of a note name such as "C4", "F#2" or "Bb3" (C4 is 60), or -1
int keyFromName(const std::string &name);
// "C4", "F#2", ... for a key
std::string keyName(int key);

// Loading. These decode audio files (WAV, FLAC, MP3) and may take a while: call them off the
// audio thread, ideally off the main thread too. On failure they return nullptr and say why
// in `error`.
//
// An SFZ file: <control>, <global>, <master>, <group> and <region> headers, #include (relative to
// the first file's folder) and #define ($NAME substituted after it). Regions use sample, key,
// lokey, hikey, pitch_keycenter, lovel, hivel, volume, amplitude, tune, transpose,
// pitch_keytrack, loop_mode with loop_start and loop_end, and ampeg_release; default_path applies.
// Only the first of round robins (seq_position, lorand), the default keyswitch (sw_default, else
// the first sw_last) and attack regions (not trigger=release) are kept. Instruments with more
// than kMaxVelocityLayers layers keep that many, spread over the range, and loading stops past
// kMaxSampleBytes of decoded audio. Each file is decoded once, to 16 bits, and shared by the
// zones that use it; near-silent tails (below about -78 dB) are trimmed from files no zone loops.
// Other opcodes are ignored.
//
// With a MemoryManager that streams, long unlooped samples keep only their first frames in RAM
// and stream the rest (see MemoryManager); a sample already in its cache is read from there
// without decoding it again. Looped samples, short ones, and everything without a manager are
// loaded whole.
constexpr int kMaxVelocityLayers = 4;
constexpr uint64_t kMaxSampleBytes = 1024ull * 1024 * 1024;
std::shared_ptr<SampleSet> loadSfz(const std::string &path, std::string &error, MemoryManager *memory = nullptr);
// The sample files an SFZ refers to, relative to its folder ('/'-separated, with any
// default_path applied), and the name in its "//+ Name:" comment if it has one. For copying
// an instrument with its samples. Reads only the SFZ (and the files it includes, listed in
// `includes` the same way), not the audio.
std::vector<std::string> sfzSamplePaths(const std::string &path, std::string &displayName, std::string &error,
                                        std::vector<std::string> *includes = nullptr);
// The .sfz file loadInstrumentFolder would load from a folder (searching two levels down when
// the folder itself has none), or empty
std::string findSfz(const std::string &folder);
// A folder of audio files each named by its note ("E2.wav", "C#4.flac"): each covers the keys
// up to halfway to its neighbours
std::shared_ptr<SampleSet> loadNoteFolder(const std::string &folder, std::string &error, MemoryManager *memory = nullptr);
// A folder holding an instrument: its .sfz file (findSfz) if it has one, else its note-named files
std::shared_ptr<SampleSet> loadInstrumentFolder(const std::string &folder, std::string &error,
                                                MemoryManager *memory = nullptr);

}
