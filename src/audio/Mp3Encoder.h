#pragma once

#include <cstdint>
#include <memory>
#include <string>

// MP3 encoding through the system's encoder: on Windows, Media Foundation's MP3 encoder (part of
// Windows 8 and later) writing a constant-bitrate MP3 file. It takes 16-bit samples at 32, 44.1 or
// 48 kHz: stereo at 96 to 320 kbps, mono at 64 to 128 kbps. The file it writes starts with an ID3v2.3 tag of its own
// (with the encoder delay and padding, for gapless playback); the export's metadata is merged into
// that tag by the MP3 writer in Encoders.cpp. Elsewhere, open says MP3 isn't available.

namespace audio {

class Mp3Encoder {
public:
    Mp3Encoder();
    ~Mp3Encoder();

    Mp3Encoder(const Mp3Encoder &) = delete;
    Mp3Encoder &operator=(const Mp3Encoder &) = delete;

    // Starts a file at `path` (UTF-8). False with the reason when the system can't encode this.
    // open, write and finish run on one thread (the encoder's COM apartment is that thread's).
    bool open(const std::string &path, uint32_t sampleRate, uint32_t channels, int kbps, std::string &error);
    // Interleaved 16-bit samples
    bool write(const int16_t *interleaved, uint64_t frames, std::string &error);
    // Flushes the encoder and closes the file
    bool finish(std::string &error);

private:
    struct State;
    std::unique_ptr<State> state_;
};

}
