#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <memory>
#include <string>

// Encoders for exports: WAV, AIFF and FLAC files written as the audio streams in. Memory stays
// bounded whatever the length (FLAC holds one block), and every file is written to `<path>.part`
// and renamed over `path` when it is closed, so a failed export never leaves a half file behind
// under the real name. FLAC is our own encoder (RFC 9639): fixed predictors of orders 0-4 with
// partitioned Rice codes, and the best of independent, left-side, right-side and mid-side stereo
// for each block.

namespace audio {

// The container and sample format an encoder writes. Integers are 16 or 24 bits; 32 is 32-bit
// float (WAV only). AIFF takes 16 or 24 bits, FLAC 16 or 24 bits.
struct EncodeFormat {
    enum Container { Wav, Aiff, Flac };
    Container container = Wav;
    int bits = 24;
    uint32_t sampleRate = 48000;
    uint32_t channels = 2;
};

// Text stored in the file. Empty fields are left out; `software` names the encoder.
struct AudioMetadata {
    std::string title;
    std::string artist;
    std::string album;
    std::string comment;
    std::string date;
    std::string software = "Vibin";
};

// Streams interleaved float audio into a file. Open refuses a format the container can't hold,
// with the reason in `error`. Integers are clamped to full scale and rounded to the nearest step
// (no dither: the Dither runs before, when wanted); 32-bit float is written as it is. write
// returns false after a failure, and close then reports the reason and removes the part file.
class AudioWriter {
public:
    virtual ~AudioWriter() = default;

    virtual bool open(const std::string &path, const EncodeFormat &format, const AudioMetadata &metadata,
                      std::string &error) = 0;
    virtual bool write(const float *interleaved, uint64_t frames) = 0;
    virtual bool close(std::string &error) = 0;
};

// The writer for the format's container, not yet open (null for an unknown container)
std::unique_ptr<AudioWriter> makeWriter(const EncodeFormat &format);

// MD5 (RFC 1321), as FLAC's STREAMINFO signs the samples. Feed it any number of pieces, then
// finish once.
class Md5 {
public:
    Md5();

    void update(const void *data, size_t size);
    std::array<uint8_t, 16> finish();

private:
    void transform(const uint8_t *block);
    void put(uint8_t byte);

    uint32_t state_[4];
    uint64_t bytes_ = 0;
    uint8_t buffer_[64] = {};
    size_t used_ = 0;
};

// A digest as 32 lowercase hex digits
std::string md5Hex(const std::array<uint8_t, 16> &digest);

// A raw temporary file of interleaved 32-bit float frames, with a small header (channels, sample
// rate, frames). Written to `<path>.part` and renamed when closed, like the encoders. Used to hold
// a render's targets until they are finished.
class FloatTempWriter {
public:
    ~FloatTempWriter();

    bool open(const std::string &path, uint32_t channels, double sampleRate, std::string &error);
    bool write(const float *interleaved, uint64_t frames);
    bool close(std::string &error);

private:
    std::ofstream out_;
    std::string path_;
    std::string part_;
    uint32_t channels_ = 0;
    uint64_t frames_ = 0;
    bool open_ = false;
};

// Reads a FloatTempWriter file back, in order (seek moves it)
class FloatTempReader {
public:
    bool open(const std::string &path, std::string &error);

    uint32_t channels() const { return channels_; }
    double sampleRate() const { return sampleRate_; }
    uint64_t frames() const { return frames_; }
    uint64_t position() const { return position_; }

    // Reads up to `frames` frames into `interleaved`; returns how many were read (0 at the end)
    uint64_t read(float *interleaved, uint64_t frames);
    void seek(uint64_t frame);

private:
    std::ifstream in_;
    uint32_t channels_ = 0;
    double sampleRate_ = 0.0;
    uint64_t frames_ = 0;
    uint64_t position_ = 0;
};

}
