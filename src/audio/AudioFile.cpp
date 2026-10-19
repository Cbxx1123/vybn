#include "AudioFile.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>

#include <miniaudio.h>

#include "Memory.h"

namespace audio {

namespace {

std::shared_ptr<AudioData> fromStream(const std::string &path, std::shared_ptr<StreamSource> source,
                                      std::vector<float> peaks)
{
    auto data = std::make_shared<AudioData>();
    data->path = path;
    data->sampleRate = source->sampleRate;
    data->channels = source->channels;
    data->frames = source->frames;
    data->peaks = std::move(peaks);
    data->stream = std::move(source);
    return data;
}

}

size_t AudioData::bytes() const
{
    const size_t preload = stream ? stream->preloadFloat->size() : 0;
    return (samples.size() + peaks.size() + preload) * sizeof(float);
}

const float *AudioData::findFrame(uint64_t index, Span &span) const
{
    if (index >= frames)
        return nullptr;
    if (!stream) {
        span = {samples.data(), 0, frames};
        return samples.data() + index * channels;
    }
    const StreamSource &s = *stream;
    if (index < s.preloadFrames) {
        span = {s.preloadFloat->data(), 0, s.preloadFrames};
        s.prefetch(0); // the preload runs out within a second
        return span.data + index * channels;
    }
    const uint32_t k = s.chunkOf(static_cast<uint32_t>(index));
    const float *chunk = s.chunkFloat(k);
    if (!chunk)
        return nullptr;
    s.prefetch(k + 1);
    const uint64_t first = s.preloadFrames + static_cast<uint64_t>(k) * s.chunkFrames;
    span = {chunk, first, std::min<uint64_t>(first + s.chunkFrames, frames)};
    return chunk + (index - first) * channels;
}

void AudioData::prefetch(uint64_t index) const
{
    if (!stream || index >= frames)
        return;
    const StreamSource &s = *stream;
    const uint32_t k = index < s.preloadFrames ? 0 : s.chunkOf(static_cast<uint32_t>(index));
    s.prefetch(k);
    s.prefetch(k + 1);
}

void PeakBuilder::add(const float *interleaved, size_t frames, uint32_t channels)
{
    for (size_t f = 0; f < frames; ++f) {
        for (uint32_t c = 0; c < channels; ++c) {
            const float v = interleaved[f * channels + c];
            low = std::min(low, v);
            high = std::max(high, v);
        }
        if (++count == AudioData::kPeakFrames) {
            peaks.push_back(low);
            peaks.push_back(high);
            low = high = 0.0f;
            count = 0;
        }
    }
}

void PeakBuilder::finish()
{
    if (count > 0) {
        peaks.push_back(low);
        peaks.push_back(high);
    }
    low = high = 0.0f;
    count = 0;
}

void AudioData::computePeaks()
{
    const uint64_t runs = (frames + kPeakFrames - 1) / kPeakFrames;
    peaks.assign(static_cast<size_t>(runs) * 2, 0.0f);
    for (uint64_t r = 0; r < runs; ++r) {
        float low = 0.0f;
        float high = 0.0f;
        const uint64_t end = std::min<uint64_t>(frames, (r + 1) * kPeakFrames);
        for (uint64_t f = r * kPeakFrames; f < end; ++f) {
            for (uint32_t c = 0; c < channels; ++c) {
                const float v = samples[static_cast<size_t>(f * channels + c)];
                low = std::min(low, v);
                high = std::max(high, v);
            }
        }
        peaks[static_cast<size_t>(r * 2)] = low;
        peaks[static_cast<size_t>(r * 2 + 1)] = high;
    }
}

void AudioData::range(double from, double to, float &low, float &high) const
{
    low = high = 0.0f;
    if (peaks.empty() || to <= from)
        return;
    const double runSeconds = kPeakFrames / sampleRate;
    const auto runs = static_cast<int64_t>(peaks.size() / 2);
    int64_t a = static_cast<int64_t>(std::floor(from / runSeconds));
    int64_t b = static_cast<int64_t>(std::ceil(to / runSeconds));
    a = std::clamp<int64_t>(a, 0, runs);
    b = std::clamp<int64_t>(b, a, runs);
    if (a == b && a < runs)
        b = a + 1;
    for (int64_t r = a; r < b; ++r) {
        low = std::min(low, peaks[static_cast<size_t>(r * 2)]);
        high = std::max(high, peaks[static_cast<size_t>(r * 2 + 1)]);
    }
}

std::shared_ptr<AudioData> loadAudioFile(const std::string &path, std::string &error, MemoryManager *memory)
{
    const bool streaming = memory && memory->streaming();
    if (streaming) {
        // Decoded before: only the preload and the overview are read
        std::vector<float> peaks;
        if (auto source = memory->openCachedFloat(path, peaks))
            return fromStream(path, std::move(source), std::move(peaks));
    }
    ma_decoder_config config = ma_decoder_config_init(ma_format_f32, 0, 0);
    ma_decoder decoder;
    if (ma_decoder_init_file(path.c_str(), &config, &decoder) != MA_SUCCESS) {
        error = "Can't open " + path + " as audio (WAV, FLAC and MP3 are supported)";
        return nullptr;
    }
    ma_format format;
    ma_uint32 channels = 0;
    ma_uint32 rate = 0;
    ma_decoder_get_data_format(&decoder, &format, &channels, &rate, nullptr, 0);
    if (channels == 0 || rate == 0) {
        ma_decoder_uninit(&decoder);
        error = "Unknown format in " + path;
        return nullptr;
    }
    const uint32_t kept = std::min<uint32_t>(channels, 2);
    ma_uint64 total = 0;
    const bool knownLength = ma_decoder_get_length_in_pcm_frames(&decoder, &total) == MA_SUCCESS && total > 0;
    if (streaming && knownLength && memory->worthStreaming(total)) {
        // Long: decoded a piece at a time into the cache, with the overview built on the way
        PeakBuilder overview;
        std::vector<float> wide;
        auto read = [&](float *out, size_t most) -> size_t {
            if (kept != channels)
                wide.resize(most * channels);
            float *into = kept == channels ? out : wide.data();
            ma_uint64 got = 0;
            ma_decoder_read_pcm_frames(&decoder, into, most, &got);
            if (kept != channels) {
                for (size_t f = 0; f < got; ++f) {
                    for (uint32_t c = 0; c < kept; ++c)
                        out[f * kept + c] = wide[f * channels + c];
                }
            }
            overview.add(out, static_cast<size_t>(got), kept);
            if (got == 0)
                overview.finish(); // before the overview is written after the audio
            return static_cast<size_t>(got);
        };
        std::string why;
        if (auto source = memory->createFloat(path, kept, rate, read, overview.peaks, why)) {
            ma_decoder_uninit(&decoder);
            return fromStream(path, std::move(source), std::move(overview.peaks));
        }
        // The cache couldn't be written: loaded whole instead
        if (ma_decoder_seek_to_pcm_frame(&decoder, 0) != MA_SUCCESS) {
            ma_decoder_uninit(&decoder);
            error = why;
            return nullptr;
        }
    }
    std::vector<float> decoded;
    if (knownLength)
        decoded.reserve(static_cast<size_t>(total * channels));
    std::vector<float> chunk(static_cast<size_t>(16384) * channels);
    for (;;) {
        ma_uint64 read = 0;
        const ma_result result = ma_decoder_read_pcm_frames(&decoder, chunk.data(), 16384, &read);
        decoded.insert(decoded.end(), chunk.begin(), chunk.begin() + static_cast<std::ptrdiff_t>(read * channels));
        if (result != MA_SUCCESS || read < 16384)
            break;
    }
    ma_decoder_uninit(&decoder);

    const size_t frames = decoded.size() / channels;
    if (frames == 0) {
        error = "No audio in " + path;
        return nullptr;
    }
    if (kept != channels) {
        std::vector<float> folded(frames * kept);
        for (size_t f = 0; f < frames; ++f) {
            for (uint32_t c = 0; c < kept; ++c)
                folded[f * kept + c] = decoded[f * channels + c];
        }
        decoded.swap(folded);
    }
    return makeAudioData(path, rate, kept, std::move(decoded));
}

std::shared_ptr<AudioData> makeAudioData(std::string path, double sampleRate, uint32_t channels,
                                         std::vector<float> interleaved, MemoryManager *memory)
{
    auto data = std::make_shared<AudioData>();
    data->path = std::move(path);
    data->sampleRate = sampleRate;
    data->channels = std::max<uint32_t>(1, channels);
    data->frames = interleaved.size() / data->channels;
    data->samples = std::move(interleaved);
    data->computePeaks();
    // A long recording plays back from its own file, which already holds these floats
    uint64_t offset = 0, fileFrames = 0;
    uint32_t fileChannels = 0;
    double fileRate = 0.0;
    if (memory && memory->streaming() && memory->worthStreaming(data->frames) &&
        floatWavLayout(data->path, offset, fileChannels, fileFrames, fileRate) && fileChannels == data->channels &&
        fileFrames == data->frames && std::lround(fileRate) == std::lround(sampleRate)) {
        const size_t keep = std::min<size_t>(data->samples.size(), static_cast<size_t>(memory->preloadFrames()) * data->channels);
        std::vector<float> preload(data->samples.begin(), data->samples.begin() + static_cast<std::ptrdiff_t>(keep));
        if (auto source = memory->openFloatFile(data->path, offset, data->channels, data->frames, sampleRate, std::move(preload))) {
            data->stream = std::move(source);
            std::vector<float>().swap(data->samples);
        }
    }
    return data;
}

bool floatWavLayout(const std::string &path, uint64_t &dataOffset, uint32_t &channels, uint64_t &frames,
                    double &sampleRate)
{
    std::ifstream in(std::filesystem::u8path(path), std::ios::binary);
    if (!in)
        return false;
    auto u16 = [](const unsigned char *p) { return static_cast<uint32_t>(p[0] | (p[1] << 8)); };
    auto u32 = [](const unsigned char *p) {
        return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) |
               (static_cast<uint32_t>(p[3]) << 24);
    };
    unsigned char riff[12];
    if (!in.read(reinterpret_cast<char *>(riff), 12) || std::memcmp(riff, "RIFF", 4) != 0 || std::memcmp(riff + 8, "WAVE", 4) != 0)
        return false;
    in.seekg(0, std::ios::end);
    const uint64_t fileSize = static_cast<uint64_t>(in.tellg());
    uint64_t at = 12;
    bool format = false;
    while (at + 8 <= fileSize) {
        unsigned char head[8];
        in.seekg(static_cast<std::streamoff>(at));
        if (!in.read(reinterpret_cast<char *>(head), 8))
            return false;
        const uint64_t size = u32(head + 4);
        if (std::memcmp(head, "fmt ", 4) == 0) {
            unsigned char fmt[40] = {};
            if (size < 16 || !in.read(reinterpret_cast<char *>(fmt), static_cast<std::streamsize>(std::min<uint64_t>(size, 40))))
                return false;
            uint32_t tag = u16(fmt);
            if (tag == 0xFFFE && size >= 26)
                tag = u16(fmt + 24); // extensible: the sub-format's first two bytes
            channels = u16(fmt + 2);
            sampleRate = u32(fmt + 4);
            if (tag != 3 || u16(fmt + 14) != 32 || channels == 0)
                return false;
            format = true;
        } else if (std::memcmp(head, "data", 4) == 0) {
            if (!format)
                return false;
            dataOffset = at + 8;
            frames = std::min(size, fileSize - dataOffset) / (static_cast<uint64_t>(channels) * sizeof(float));
            return true;
        }
        at += 8 + size + (size & 1);
    }
    return false;
}

bool writeWavFile(const std::string &path, const float *interleaved, uint64_t frames, uint32_t channels,
                  double sampleRate, std::string &error, int bits)
{
    const ma_format format = bits == 16 ? ma_format_s16 : bits == 24 ? ma_format_s24 : ma_format_f32;
    ma_encoder_config config = ma_encoder_config_init(ma_encoding_format_wav, format, channels,
                                                      static_cast<ma_uint32>(sampleRate));
    ma_encoder encoder;
    if (ma_encoder_init_file(path.c_str(), &config, &encoder) != MA_SUCCESS) {
        error = "Couldn't create " + path;
        return false;
    }
    // Integers are converted a piece at a time, so a long file needs no second copy
    constexpr uint64_t kPiece = 8192;
    std::vector<unsigned char> buffer;
    uint32_t noise = 22222;
    auto random = [&noise] {
        noise = noise * 1664525u + 1013904223u;
        return static_cast<float>(noise >> 8) / 16777216.0f; // 0-1
    };
    bool ok = true;
    for (uint64_t at = 0; at < frames && ok; at += (format == ma_format_f32 ? frames : kPiece)) {
        const uint64_t n = format == ma_format_f32 ? frames : std::min(kPiece, frames - at);
        const float *src = interleaved + at * channels;
        const void *data = src;
        const size_t values = static_cast<size_t>(n * channels);
        if (format == ma_format_s16) {
            buffer.resize(values * 2);
            auto *out = reinterpret_cast<int16_t *>(buffer.data());
            for (size_t i = 0; i < values; ++i) {
                const float dither = random() - random(); // triangular, one step wide each way
                const float v = std::round(src[i] * 32768.0f + dither);
                out[i] = static_cast<int16_t>(std::clamp(v, -32768.0f, 32767.0f));
            }
            data = buffer.data();
        } else if (format == ma_format_s24) {
            buffer.resize(values * 3);
            for (size_t i = 0; i < values; ++i) {
                const double v = std::round(static_cast<double>(src[i]) * 8388608.0);
                const auto s24 = static_cast<int32_t>(std::clamp(v, -8388608.0, 8388607.0));
                buffer[i * 3 + 0] = static_cast<unsigned char>(s24 & 0xff);
                buffer[i * 3 + 1] = static_cast<unsigned char>((s24 >> 8) & 0xff);
                buffer[i * 3 + 2] = static_cast<unsigned char>((s24 >> 16) & 0xff);
            }
            data = buffer.data();
        }
        ma_uint64 written = 0;
        ok = ma_encoder_write_pcm_frames(&encoder, data, n, &written) == MA_SUCCESS && written == n;
    }
    ma_encoder_uninit(&encoder);
    if (!ok) {
        error = "Couldn't write all of " + path;
        return false;
    }
    return true;
}

}
