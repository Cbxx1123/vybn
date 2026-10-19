#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace audio {

class MemoryManager;
class StreamSource;

// A sound file: mono or stereo interleaved f32 at the file's own rate, with an overview of its
// peaks for drawing. Short files are wholly in `samples`; long ones, when the MemoryManager
// streams, keep only the overview and a preload in RAM and read the rest from disk as they play
// (`stream`). Immutable once made, and shared (clips that use the same file share one), so it can
// be read on the audio thread while the main thread holds it too.
struct AudioData {
    std::string path;
    double sampleRate = 48000.0;
    uint32_t channels = 1;
    uint64_t frames = 0;
    std::vector<float> samples; // interleaved; empty when streamed
    std::shared_ptr<const StreamSource> stream;

    // A run of frames in memory, [first, end), as the audio thread last found it: the whole file,
    // the preload or one streamed chunk. Its pointer is good only within one audio callback, so
    // readers start each block with a fresh Span.
    struct Span {
        const float *data = nullptr;
        uint64_t first = 0;
        uint64_t end = 0;
    };
    // Real-time: frame `index` (interleaved channels), or null past the end or while its chunk
    // isn't in memory (it has then been asked for, and counted as an underrun). Entering a
    // streamed run asks for the chunk after it.
    const float *frameAt(uint64_t index, Span &span) const
    {
        if (index >= span.first && index < span.end)
            return span.data + (index - span.first) * channels;
        return findFrame(index, span);
    }
    // Real-time: asks for the chunks holding `index` and the frames after it, ahead of playing
    void prefetch(uint64_t index) const;
    bool streamed() const { return stream != nullptr; }

    // Peaks for drawing: for each run of kPeakFrames frames, the lowest and highest sample of any
    // channel (pairs: min, max)
    static constexpr uint32_t kPeakFrames = 256;
    std::vector<float> peaks;

    double seconds() const { return sampleRate > 0.0 ? static_cast<double>(frames) / sampleRate : 0.0; }
    // RAM it holds (a streamed file's preload and overview; its chunks are the MemoryManager's)
    size_t bytes() const;
    // The lowest and highest sample from `from` to `to` seconds, using the overview
    void range(double from, double to, float &low, float &high) const;
    // Fills peaks from samples
    void computePeaks();

private:
    const float *findFrame(uint64_t index, Span &span) const;
};

// Builds an overview a piece at a time, the same as AudioData::computePeaks
struct PeakBuilder {
    std::vector<float> peaks;
    float low = 0.0f, high = 0.0f;
    uint32_t count = 0;
    void add(const float *interleaved, size_t frames, uint32_t channels);
    void finish(); // the last, partial run
};

// Decodes a WAV, FLAC or MP3 file (miniaudio's decoders) to mono or stereo (more channels fold to
// their first two), with peaks. For a background thread: it reads the whole file. Returns null
// with the reason in error. With a MemoryManager that streams, a long file is streamed: the
// cache's float copy is read if it is up to date (only its preload and overview, no decoding),
// else the file is decoded a piece at a time into the cache, so it is never whole in memory.
std::shared_ptr<AudioData> loadAudioFile(const std::string &path, std::string &error, MemoryManager *memory = nullptr);

// Makes AudioData from samples already in memory (a recording), with peaks. With a MemoryManager
// that streams, a long recording whose file at `path` is the 32-bit float WAV of these samples
// (as writeWavFile writes them) streams from that file, and the samples are let go.
std::shared_ptr<AudioData> makeAudioData(std::string path, double sampleRate, uint32_t channels,
                                         std::vector<float> interleaved, MemoryManager *memory = nullptr);

// Where a 32-bit float PCM WAV file's audio starts, and its layout; false if it isn't one
bool floatWavLayout(const std::string &path, uint64_t &dataOffset, uint32_t &channels, uint64_t &frames,
                    double &sampleRate);

// Writes interleaved samples as a WAV file: 32-bit float, or 24- or 16-bit integers (clipped to
// full scale; 16-bit is dithered with triangular noise)
bool writeWavFile(const std::string &path, const float *interleaved, uint64_t frames, uint32_t channels,
                  double sampleRate, std::string &error, int bits = 32);

// The file types the decoders read, as open-dialog patterns
constexpr const char *kAudioFilePatterns = "*.wav;*.flac;*.mp3";

}
