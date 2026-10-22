#include "Encoders.h"

#include "Mp3Encoder.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <system_error>
#include <utility>
#include <vector>

namespace audio {

namespace {

// FLAC codes a block of this many frames at once; the last block of a file may be shorter
constexpr size_t kBlock = 4096;
// Frames converted per write to a WAV or AIFF file, so a long write needs no big buffer
constexpr uint64_t kPiece = 8192;
// The most data a WAV or AIFF file may hold while its sizes stay 32-bit
constexpr uint64_t kMaxPcmBytes = 0xFFFFF000ull;

// One integer sample as the file holds it: clamped to full scale, scaled by 2^(bits-1), rounded
// to the nearest step (the same conversion writeWavFile makes, without its dither)
int32_t toInt(float value, int bits)
{
    const double full = static_cast<double>(1ll << (bits - 1));
    const double v = std::isnan(value) ? 0.0 : std::clamp(static_cast<double>(value), -1.0, 1.0);
    return static_cast<int32_t>(std::clamp(std::round(v * full), -full, full - 1.0));
}

// Why a format can't be written, or nothing
std::string refusal(const EncodeFormat &format)
{
    if (format.channels < 1 || format.channels > 8)
        return "Only 1 to 8 channels can be written";
    if (format.sampleRate == 0)
        return "The sample rate must be above zero";
    const std::string bits = std::to_string(format.bits) + "-bit";
    switch (format.container) {
    case EncodeFormat::Wav:
        if (format.bits == 16 || format.bits == 24 || format.bits == 32)
            return {};
        return "WAV is written as 16 or 24-bit integers or 32-bit float, not " + bits;
    case EncodeFormat::Aiff:
        if (format.bits == 16 || format.bits == 24)
            return {};
        return "AIFF is written as 16 or 24-bit integers, not " + bits;
    case EncodeFormat::Flac:
        if (format.sampleRate > 655350)
            return "FLAC can't hold a sample rate above 655350 Hz";
        if (format.bits == 16 || format.bits == 24)
            return {};
        return "FLAC is written as 16 or 24-bit integers, not " + bits;
    case EncodeFormat::Mp3:
        if (format.channels > 2)
            return "MP3 holds mono or stereo";
        if (std::find(std::begin(kMp3Rates), std::end(kMp3Rates), format.sampleRate) == std::end(kMp3Rates))
            return "MP3 is written at 32, 44.1 or 48 kHz, not " + std::to_string(format.sampleRate) + " Hz";
        if (std::find(std::begin(kMp3Bitrates), std::end(kMp3Bitrates), format.bitrate) == std::end(kMp3Bitrates))
            return "MP3 is written at 96 to 320 kbps (a standard bitrate), not " + std::to_string(format.bitrate);
        if (format.bits == 16)
            return {};
        return "MP3 is encoded from 16-bit samples, not " + bits;
    }
    return "Unknown container";
}

// Appends `count` bytes of `value`
void putBytes(std::vector<uint8_t> &out, uint64_t value, int count, bool bigEndian)
{
    for (int i = 0; i < count; ++i) {
        const int shift = 8 * (bigEndian ? count - 1 - i : i);
        out.push_back(static_cast<uint8_t>(value >> shift));
    }
}

void putText(std::vector<uint8_t> &out, const std::string &text)
{
    out.insert(out.end(), text.begin(), text.end());
}

// A RIFF INFO entry (NUL terminated) or an AIFF text chunk; both are padded to an even size
void putTextChunk(std::vector<uint8_t> &out, const char *id, const std::string &text, bool bigEndian, bool nul)
{
    if (text.empty())
        return;
    const size_t size = text.size() + (nul ? 1 : 0);
    putText(out, id);
    putBytes(out, size, 4, bigEndian);
    putText(out, text);
    if (nul)
        out.push_back(0);
    if (size & 1)
        out.push_back(0);
}

// The sample rate as an 80-bit IEEE extended float (a sign, a 15-bit exponent and a mantissa
// with its explicit leading one), big-endian, as AIFF stores it
void putExtended(std::vector<uint8_t> &out, uint32_t rate)
{
    int exponent = 0;
    while (exponent < 63 && (static_cast<uint64_t>(rate) >> (exponent + 1)) != 0)
        ++exponent;
    const uint64_t mantissa = static_cast<uint64_t>(rate) << (63 - exponent);
    putBytes(out, static_cast<uint64_t>(16383 + exponent), 2, true);
    putBytes(out, mantissa, 8, true);
}

// The integer samples of `values` as bytes: little-endian (WAV) or big-endian (AIFF). 32-bit
// float is written as its bit pattern.
void pcmBytes(const float *in, size_t values, int bits, bool bigEndian, uint8_t *out)
{
    const size_t bytes = static_cast<size_t>(bits / 8);
    for (size_t i = 0; i < values; ++i) {
        uint32_t raw = 0;
        if (bits == 32) {
            std::memcpy(&raw, &in[i], sizeof raw);
        } else {
            raw = static_cast<uint32_t>(toInt(in[i], bits));
        }
        for (size_t b = 0; b < bytes; ++b) {
            const size_t shift = 8 * (bigEndian ? bytes - 1 - b : b);
            out[i * bytes + b] = static_cast<uint8_t>(raw >> shift);
        }
    }
}

// The Vorbis comments a FLAC file carries: each non-empty field as NAME=value
std::vector<std::string> vorbisComments(const AudioMetadata &m)
{
    const std::pair<const char *, const std::string *> fields[] = {
        {"TITLE", &m.title}, {"ARTIST", &m.artist}, {"ALBUM", &m.album},
        {"COMMENT", &m.comment}, {"DATE", &m.date}, {"ENCODER", &m.software},
    };
    std::vector<std::string> out;
    for (const auto &[key, value] : fields) {
        if (!value->empty())
            out.push_back(std::string(key) + "=" + *value);
    }
    return out;
}

// Shared file handling: the part file, the rename on close, and the failure reason. Each
// container writes its own header, samples and sizes through the hooks below.
class FileWriter : public AudioWriter {
public:
    ~FileWriter() override
    {
        if (out_.is_open()) {
            out_.close();
            removePart();
        }
    }

    bool open(const std::string &path, const EncodeFormat &format, const AudioMetadata &metadata,
              std::string &error) override
    {
        std::string why = refusal(format);
        if (!why.empty()) {
            error = why;
            return false;
        }
        format_ = format;
        metadata_ = metadata;
        path_ = path;
        part_ = path + ".part";
        out_.open(std::filesystem::u8path(part_), std::ios::binary | std::ios::trunc);
        if (!out_) {
            error = "Couldn't create " + path;
            return false;
        }
        if (!begin(why)) {
            out_.close();
            removePart();
            error = why;
            return false;
        }
        writing_ = true;
        return true;
    }

    bool write(const float *interleaved, uint64_t frames) override
    {
        if (!writing_)
            return false;
        if (frames == 0)
            return true;
        std::string why;
        if (encode(interleaved, frames, why))
            return true;
        error_ = why;
        writing_ = false;
        return false;
    }

    bool close(std::string &error) override
    {
        if (!out_.is_open()) {
            error = error_.empty() ? "The file isn't open" : error_;
            return false;
        }
        std::string why = error_;
        bool ok = writing_;
        if (ok && !finish(why))
            ok = false;
        if (ok) {
            out_.flush();
            if (!out_) {
                ok = false;
                why = "Couldn't write " + path_;
            }
        }
        out_.close();
        writing_ = false;
        if (ok) {
            // Replaces a file already there (rename alone won't on Windows)
            std::error_code ec;
            std::filesystem::remove(std::filesystem::u8path(path_), ec);
            std::filesystem::rename(std::filesystem::u8path(part_), std::filesystem::u8path(path_), ec);
            if (ec) {
                ok = false;
                why = "Couldn't save " + path_;
            }
        }
        if (!ok) {
            removePart();
            error = why.empty() ? "Couldn't write " + path_ : why;
            return false;
        }
        return true;
    }

protected:
    // Writes the header; the file is open
    virtual bool begin(std::string &why) = 0;
    // Writes frames of interleaved floats
    virtual bool encode(const float *interleaved, uint64_t frames, std::string &why) = 0;
    // Writes whatever sizes and tails the header needs, once everything is written
    virtual bool finish(std::string &why) = 0;

    // Overwrites bytes already written at `at`, then returns to the end
    void patch(std::streamoff at, const std::vector<uint8_t> &bytes)
    {
        out_.seekp(at);
        out_.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        out_.seekp(0, std::ios::end);
    }

    EncodeFormat format_;
    AudioMetadata metadata_;
    std::string path_;
    std::string part_;
    std::ofstream out_;
    std::vector<uint8_t> buffer_;

private:
    void removePart()
    {
        std::error_code ec;
        std::filesystem::remove(std::filesystem::u8path(part_), ec);
    }

    bool writing_ = false;
    std::string error_;
};

class WavWriter : public FileWriter {
protected:
    bool begin(std::string &why) override
    {
        (void)why;
        const bool floatData = format_.bits == 32;
        const bool extensible = format_.bits != 16 || format_.channels > 2;
        const uint32_t channels = format_.channels;
        const uint32_t blockAlign = channels * static_cast<uint32_t>(format_.bits / 8);
        const uint32_t mask = channels == 1 ? 0x4u : channels == 2 ? 0x3u : (1u << channels) - 1u;

        std::vector<uint8_t> h;
        putText(h, "RIFF");
        putBytes(h, 0, 4, false); // the RIFF size, set on close
        putText(h, "WAVE");
        putText(h, "fmt ");
        putBytes(h, extensible ? 40 : 16, 4, false);
        putBytes(h, extensible ? 0xFFFE : 1, 2, false);
        putBytes(h, channels, 2, false);
        putBytes(h, format_.sampleRate, 4, false);
        putBytes(h, static_cast<uint64_t>(format_.sampleRate) * blockAlign, 4, false);
        putBytes(h, blockAlign, 2, false);
        putBytes(h, static_cast<uint64_t>(format_.bits), 2, false);
        if (extensible) {
            // The sub-format GUID: PCM (1) or IEEE float (3), with the usual suffix
            putBytes(h, 22, 2, false);
            putBytes(h, static_cast<uint64_t>(format_.bits), 2, false);
            putBytes(h, mask, 4, false);
            putBytes(h, floatData ? 3 : 1, 4, false);
            putBytes(h, 0, 2, false);
            putBytes(h, 0x10, 2, false);
            const uint8_t suffix[8] = {0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71};
            h.insert(h.end(), suffix, suffix + 8);
        }
        // The metadata, as a LIST chunk of INFO entries (always with the encoder's name)
        std::vector<uint8_t> info;
        putTextChunk(info, "INAM", metadata_.title, false, true);
        putTextChunk(info, "IART", metadata_.artist, false, true);
        putTextChunk(info, "IPRD", metadata_.album, false, true);
        putTextChunk(info, "ICMT", metadata_.comment, false, true);
        putTextChunk(info, "ICRD", metadata_.date, false, true);
        putTextChunk(info, "ISFT", metadata_.software, false, true);
        putText(h, "LIST");
        putBytes(h, 4 + info.size(), 4, false);
        putText(h, "INFO");
        h.insert(h.end(), info.begin(), info.end());
        putText(h, "data");
        dataSizeAt_ = static_cast<std::streamoff>(h.size());
        putBytes(h, 0, 4, false); // the data size, set on close
        out_.write(reinterpret_cast<const char *>(h.data()), static_cast<std::streamsize>(h.size()));
        return static_cast<bool>(out_);
    }

    bool encode(const float *in, uint64_t frames, std::string &why) override
    {
        const size_t bytes = static_cast<size_t>(format_.bits / 8);
        for (uint64_t at = 0; at < frames; at += kPiece) {
            const uint64_t n = std::min(kPiece, frames - at);
            const size_t values = static_cast<size_t>(n * format_.channels);
            if (dataBytes_ + values * bytes > kMaxPcmBytes) {
                why = "WAV files over 4 GB aren't supported";
                return false;
            }
            buffer_.resize(values * bytes);
            pcmBytes(in + at * format_.channels, values, format_.bits, false, buffer_.data());
            out_.write(reinterpret_cast<const char *>(buffer_.data()), static_cast<std::streamsize>(buffer_.size()));
            dataBytes_ += buffer_.size();
            if (!out_) {
                why = "Couldn't write " + path_;
                return false;
            }
        }
        return true;
    }

    bool finish(std::string &why) override
    {
        (void)why;
        if (dataBytes_ & 1)
            out_.put(0); // the chunk's pad byte, not counted in its size
        const auto end = static_cast<std::streamoff>(out_.tellp());
        std::vector<uint8_t> riff, data;
        putBytes(riff, static_cast<uint64_t>(end - 8), 4, false);
        putBytes(data, dataBytes_, 4, false);
        patch(4, riff);
        patch(dataSizeAt_, data);
        return true;
    }

private:
    uint64_t dataBytes_ = 0;
    std::streamoff dataSizeAt_ = 0;
};

class AiffWriter : public FileWriter {
protected:
    bool begin(std::string &why) override
    {
        (void)why;
        const uint32_t channels = format_.channels;
        const uint32_t bytes = static_cast<uint32_t>(format_.bits / 8);
        std::vector<uint8_t> h;
        putText(h, "FORM");
        putBytes(h, 0, 4, true); // the form size, set on close
        putText(h, "AIFF");
        putText(h, "COMM");
        putBytes(h, 18, 4, true);
        putBytes(h, channels, 2, true);
        commFramesAt_ = static_cast<std::streamoff>(h.size());
        putBytes(h, 0, 4, true); // the frame count, set on close
        putBytes(h, static_cast<uint64_t>(format_.bits), 2, true);
        putExtended(h, format_.sampleRate);
        putTextChunk(h, "NAME", metadata_.title, true, false);
        putTextChunk(h, "AUTH", metadata_.artist, true, false);
        putTextChunk(h, "ANNO", metadata_.comment, true, false);
        if (!metadata_.album.empty())
            putTextChunk(h, "ANNO", "Album: " + metadata_.album, true, false);
        if (!metadata_.date.empty())
            putTextChunk(h, "ANNO", "Date: " + metadata_.date, true, false);
        putTextChunk(h, "ANNO", "Encoded by " + metadata_.software, true, false);
        putText(h, "SSND");
        ssndSizeAt_ = static_cast<std::streamoff>(h.size());
        putBytes(h, 0, 4, true); // the chunk size, set on close
        putBytes(h, 0, 4, true); // the offset of the first sample
        putBytes(h, 0, 4, true); // the block size
        out_.write(reinterpret_cast<const char *>(h.data()), static_cast<std::streamsize>(h.size()));
        frameBytes_ = channels * bytes;
        return static_cast<bool>(out_);
    }

    bool encode(const float *in, uint64_t frames, std::string &why) override
    {
        const size_t bytes = static_cast<size_t>(format_.bits / 8);
        for (uint64_t at = 0; at < frames; at += kPiece) {
            const uint64_t n = std::min(kPiece, frames - at);
            const size_t values = static_cast<size_t>(n * format_.channels);
            if (dataBytes_ + values * bytes > kMaxPcmBytes) {
                why = "AIFF files over 4 GB aren't supported";
                return false;
            }
            buffer_.resize(values * bytes);
            pcmBytes(in + at * format_.channels, values, format_.bits, true, buffer_.data());
            out_.write(reinterpret_cast<const char *>(buffer_.data()), static_cast<std::streamsize>(buffer_.size()));
            dataBytes_ += buffer_.size();
            if (!out_) {
                why = "Couldn't write " + path_;
                return false;
            }
        }
        return true;
    }

    bool finish(std::string &why) override
    {
        (void)why;
        if (dataBytes_ & 1)
            out_.put(0); // the SSND chunk's pad byte
        const auto end = static_cast<std::streamoff>(out_.tellp());
        std::vector<uint8_t> form, ssnd, frames;
        putBytes(form, static_cast<uint64_t>(end - 8), 4, true);
        putBytes(ssnd, 8 + dataBytes_, 4, true);
        putBytes(frames, dataBytes_ / frameBytes_, 4, true);
        patch(4, form);
        patch(ssndSizeAt_, ssnd);
        patch(commFramesAt_, frames);
        return true;
    }

private:
    uint64_t dataBytes_ = 0;
    uint32_t frameBytes_ = 1;
    std::streamoff commFramesAt_ = 0;
    std::streamoff ssndSizeAt_ = 0;
};

// The MSB-first bits of a FLAC frame, with the bytes they make up
class BitWriter {
public:
    std::vector<uint8_t> bytes;

    void clear()
    {
        bytes.clear();
        acc_ = 0;
        count_ = 0;
    }
    void bit(uint64_t value)
    {
        acc_ = static_cast<uint8_t>((acc_ << 1) | (value & 1));
        if (++count_ == 8) {
            bytes.push_back(acc_);
            acc_ = 0;
            count_ = 0;
        }
    }
    // The low `count` bits of `value`, most significant first
    void bits(uint64_t value, int count)
    {
        for (int i = count - 1; i >= 0; --i)
            bit(value >> i);
    }
    void zeros(uint64_t count)
    {
        for (uint64_t i = 0; i < count; ++i)
            bit(0);
    }
    // Pads with zeros to a whole byte
    void align()
    {
        while (count_ != 0)
            bit(0);
    }

private:
    uint8_t acc_ = 0;
    int count_ = 0;
};

// How one channel of a block is coded: CONSTANT (one value), VERBATIM (the samples), or FIXED
// (a predictor of `order`, with the residuals in 2^partOrder partitions of Rice codes)
struct Plan {
    enum Kind { Constant, Verbatim, Fixed };
    Kind kind = Verbatim;
    int order = 0;
    int partOrder = 0;
    std::vector<int> params;
    uint64_t bits = 0;
};

// The fixed predictor of `order` for sample i (the samples before it are in x)
int64_t predict(const int32_t *x, size_t i, int order)
{
    const int64_t a = x[i - 1];
    switch (order) {
    case 0:
        return 0;
    case 1:
        return a;
    case 2:
        return 2 * a - x[i - 2];
    case 3:
        return 3 * a - 3 * static_cast<int64_t>(x[i - 2]) + x[i - 3];
    default:
        return 4 * a - 6 * static_cast<int64_t>(x[i - 2]) + 4 * static_cast<int64_t>(x[i - 3]) - x[i - 4];
    }
}

// A signed residual as the unsigned value Rice codes: 0, -1, 1, -2, ... become 0, 1, 2, 3, ...
uint64_t zigzag(int64_t r)
{
    return r >= 0 ? static_cast<uint64_t>(r) * 2 : static_cast<uint64_t>(-(r + 1)) * 2 + 1;
}

// The cheapest Rice parameter (0-14) for these values, and what it costs in bits
uint64_t riceCost(const uint64_t *u, size_t count, int &bestParam)
{
    uint64_t sum = 0;
    for (size_t i = 0; i < count; ++i)
        sum += u[i];
    const uint64_t mean = count ? sum / count : 0;
    int around = 0;
    while (around < 14 && (mean >> (around + 1)) != 0)
        ++around;
    uint64_t best = UINT64_MAX;
    for (int k = std::max(0, around - 1); k <= std::min(14, around + 1); ++k) {
        uint64_t cost = count * static_cast<uint64_t>(1 + k);
        for (size_t i = 0; i < count; ++i)
            cost += u[i] >> k;
        if (cost < best) {
            best = cost;
            bestParam = k;
        }
    }
    return best;
}

// The cheapest way to code `n` samples at `bps` bits (n is at least 1)
Plan planSubframe(const int32_t *x, size_t n, uint32_t bps)
{
    bool constant = true;
    for (size_t i = 1; i < n && constant; ++i)
        constant = x[i] == x[0];
    if (constant) {
        Plan c;
        c.kind = Plan::Constant;
        c.bits = 8 + bps;
        return c;
    }
    Plan best;
    best.kind = Plan::Verbatim;
    best.bits = 8 + static_cast<uint64_t>(n) * bps;
    std::vector<uint64_t> u(n);
    for (int order = 0; order <= 4 && static_cast<size_t>(order) <= n; ++order) {
        for (size_t i = static_cast<size_t>(order); i < n; ++i)
            u[i - static_cast<size_t>(order)] = zigzag(x[i] - predict(x, i, order));
        for (int partOrder = 0; partOrder <= 6; ++partOrder) {
            const size_t parts = size_t(1) << partOrder;
            if (n % parts != 0 || (n >> partOrder) <= static_cast<size_t>(order))
                break;
            const size_t m = n >> partOrder;
            // Header, warm-up samples, the Rice method and the partition order
            uint64_t bits = 8 + static_cast<uint64_t>(order) * bps + 6;
            std::vector<int> params;
            size_t at = 0;
            for (size_t p = 0; p < parts; ++p) {
                const size_t len = p == 0 ? m - static_cast<size_t>(order) : m;
                int k = 0;
                bits += 4 + riceCost(&u[at], len, k);
                params.push_back(k);
                at += len;
            }
            if (bits < best.bits) {
                best.kind = Plan::Fixed;
                best.order = order;
                best.partOrder = partOrder;
                best.params = std::move(params);
                best.bits = bits;
            }
        }
    }
    return best;
}

// Writes a subframe of `n` samples as its plan says
void emitSubframe(BitWriter &w, const Plan &plan, const int32_t *x, size_t n, uint32_t bps)
{
    const uint64_t mask = (uint64_t(1) << bps) - 1;
    if (plan.kind == Plan::Constant) {
        w.bits(0, 8); // padding, CONSTANT, no wasted bits
        w.bits(static_cast<uint64_t>(x[0]) & mask, static_cast<int>(bps));
        return;
    }
    if (plan.kind == Plan::Verbatim) {
        w.bits(0x02, 8); // padding, VERBATIM, no wasted bits
        for (size_t i = 0; i < n; ++i)
            w.bits(static_cast<uint64_t>(x[i]) & mask, static_cast<int>(bps));
        return;
    }
    const int order = plan.order;
    w.bits(static_cast<uint64_t>(8 + order) << 1, 8); // padding, FIXED of this order, no wasted bits
    for (int i = 0; i < order; ++i)
        w.bits(static_cast<uint64_t>(x[i]) & mask, static_cast<int>(bps));
    w.bits(0, 2); // Rice codes with 4-bit parameters
    w.bits(static_cast<uint64_t>(plan.partOrder), 4);
    const size_t parts = size_t(1) << plan.partOrder;
    const size_t m = n >> plan.partOrder;
    size_t i = static_cast<size_t>(order);
    for (size_t p = 0; p < parts; ++p) {
        const size_t len = p == 0 ? m - static_cast<size_t>(order) : m;
        const int k = plan.params[p];
        w.bits(static_cast<uint64_t>(k), 4);
        for (size_t end = i + len; i < end; ++i) {
            const uint64_t u = zigzag(x[i] - predict(x, i, order));
            w.zeros(u >> k);
            w.bit(1);
            w.bits(u & ((uint64_t(1) << k) - 1), k);
        }
    }
}

// The CRC-8 of a FLAC frame header (polynomial x^8 + x^2 + x + 1)
uint8_t crc8(const uint8_t *data, size_t size)
{
    uint8_t crc = 0;
    for (size_t i = 0; i < size; ++i) {
        crc ^= data[i];
        for (int b = 0; b < 8; ++b)
            crc = static_cast<uint8_t>((crc & 0x80) ? (crc << 1) ^ 0x07 : crc << 1);
    }
    return crc;
}

// The CRC-16 of a whole FLAC frame (polynomial x^16 + x^15 + x^2 + 1)
uint16_t crc16(const uint8_t *data, size_t size)
{
    uint16_t crc = 0;
    for (size_t i = 0; i < size; ++i) {
        crc ^= static_cast<uint16_t>(data[i] << 8);
        for (int b = 0; b < 8; ++b)
            crc = static_cast<uint16_t>((crc & 0x8000) ? (crc << 1) ^ 0x8005 : crc << 1);
    }
    return crc;
}

// A frame number in FLAC's UTF-8 form (up to 36 bits)
void putUtf8(BitWriter &w, uint64_t value)
{
    if (value < 0x80) {
        w.bits(value, 8);
        return;
    }
    int count = 2; // bytes; a count of n holds 5n + 1 bits
    while (count < 7 && (value >> (5 * count + 1)) != 0)
        ++count;
    w.bits(((0xFFull << (8 - count)) & 0xFF) | (value >> (6 * (count - 1))), 8);
    for (int k = count - 2; k >= 0; --k)
        w.bits(0x80 | ((value >> (6 * k)) & 0x3F), 8);
}

class FlacWriter : public FileWriter {
protected:
    bool begin(std::string &why) override
    {
        (void)why;
        std::vector<uint8_t> h;
        putText(h, "fLaC");
        // STREAMINFO (type 0, not the last block), 34 bytes filled in on close
        putBytes(h, 0x00, 1, false);
        putBytes(h, 34, 3, true);
        streamInfoAt_ = static_cast<std::streamoff>(h.size());
        h.insert(h.end(), 34, 0);
        // VORBIS_COMMENT (type 4, the last block): the vendor, then the comments
        std::vector<uint8_t> body;
        const std::string vendor = "Vibin";
        putBytes(body, vendor.size(), 4, false);
        putText(body, vendor);
        const std::vector<std::string> comments = vorbisComments(metadata_);
        putBytes(body, comments.size(), 4, false);
        for (const std::string &c : comments) {
            putBytes(body, c.size(), 4, false);
            putText(body, c);
        }
        putBytes(h, 0x84, 1, false);
        putBytes(h, body.size(), 3, true);
        h.insert(h.end(), body.begin(), body.end());
        out_.write(reinterpret_cast<const char *>(h.data()), static_cast<std::streamsize>(h.size()));

        block_.assign(format_.channels, std::vector<int32_t>(kBlock));
        side_.assign(kBlock, 0);
        mid_.assign(kBlock, 0);
        md5Bytes_.resize(kBlock * format_.channels * static_cast<size_t>(format_.bits / 8));
        return static_cast<bool>(out_);
    }

    bool encode(const float *in, uint64_t frames, std::string &why) override
    {
        const uint32_t channels = format_.channels;
        for (uint64_t f = 0; f < frames; ++f) {
            for (uint32_t c = 0; c < channels; ++c)
                block_[c][fill_] = toInt(in[f * channels + c], format_.bits);
            if (++fill_ == kBlock) {
                if (!emit(kBlock, why))
                    return false;
                fill_ = 0;
            }
        }
        return true;
    }

    bool finish(std::string &why) override
    {
        if (fill_ > 0 && !emit(fill_, why))
            return false;
        // STREAMINFO: the block sizes, frame sizes, rate, channels, depth, total and signature
        BitWriter s;
        // The minimum block size leaves out the last block (RFC 9639): every other block is full, so
        // it is the fixed size when there is more than one block, else the one block's own size
        s.bits(frameIndex_ > 1 ? kBlock : (maxBlock_ ? maxBlock_ : kBlock), 16);
        s.bits(maxBlock_ ? maxBlock_ : kBlock, 16);
        s.bits(minFrame_, 24);
        s.bits(maxFrame_, 24);
        s.bits(format_.sampleRate, 20);
        s.bits(format_.channels - 1, 3);
        s.bits(static_cast<uint64_t>(format_.bits - 1), 5);
        s.bits(total_, 36);
        std::vector<uint8_t> info = s.bytes;
        const std::array<uint8_t, 16> digest = md5_.finish();
        info.insert(info.end(), digest.begin(), digest.end());
        out_.seekp(streamInfoAt_);
        out_.write(reinterpret_cast<const char *>(info.data()), static_cast<std::streamsize>(info.size()));
        out_.seekp(0, std::ios::end);
        return true;
    }

private:
    // One channel's coding in a frame: the samples, their depth and the plan
    struct Subframe {
        const int32_t *x;
        uint32_t bps;
        Plan plan;
    };

    // Codes the first n samples of the block as one frame
    bool emit(size_t n, std::string &why)
    {
        const uint32_t bps = static_cast<uint32_t>(format_.bits);
        const uint32_t channels = format_.channels;
        std::vector<Subframe> parts;
        int assign = static_cast<int>(channels) - 1; // independent channels
        if (channels == 2) {
            const int32_t *left = block_[0].data();
            const int32_t *right = block_[1].data();
            for (size_t i = 0; i < n; ++i) {
                side_[i] = left[i] - right[i];
                mid_[i] = (left[i] + right[i]) >> 1;
            }
            const Plan pl = planSubframe(left, n, bps);
            const Plan pr = planSubframe(right, n, bps);
            const Plan ps = planSubframe(side_.data(), n, bps + 1);
            const Plan pm = planSubframe(mid_.data(), n, bps);
            uint64_t best = pl.bits + pr.bits;
            if (pl.bits + ps.bits < best) {
                best = pl.bits + ps.bits;
                assign = 8; // left-side
            }
            if (ps.bits + pr.bits < best) {
                best = ps.bits + pr.bits;
                assign = 9; // side-right
            }
            if (pm.bits + ps.bits < best) {
                assign = 10; // mid-side
            }
            switch (assign) {
            case 8:
                parts = {{left, bps, pl}, {side_.data(), bps + 1, ps}};
                break;
            case 9:
                parts = {{side_.data(), bps + 1, ps}, {right, bps, pr}};
                break;
            case 10:
                parts = {{mid_.data(), bps, pm}, {side_.data(), bps + 1, ps}};
                break;
            default:
                parts = {{left, bps, pl}, {right, bps, pr}};
                break;
            }
        } else {
            for (uint32_t c = 0; c < channels; ++c) {
                const int32_t *x = block_[c].data();
                parts.push_back({x, bps, planSubframe(x, n, bps)});
            }
        }

        frame_.clear();
        const uint32_t blockCode = n == kBlock ? 0xC : n <= 256 ? 0x6 : 0x7;
        const uint32_t sizeCode = bps == 16 ? 4 : 6;
        frame_.bits(0xFF, 8); // the sync code and its two flags
        frame_.bits(0xF8, 8);
        frame_.bits(blockCode, 4);
        frame_.bits(0, 4); // the rate is in STREAMINFO
        frame_.bits(static_cast<uint64_t>(assign), 4);
        frame_.bits(sizeCode, 3);
        frame_.bits(0, 1);
        putUtf8(frame_, frameIndex_);
        if (blockCode == 0x6)
            frame_.bits(n - 1, 8);
        else if (blockCode == 0x7)
            frame_.bits(n - 1, 16);
        frame_.bits(crc8(frame_.bytes.data(), frame_.bytes.size()), 8);
        for (const Subframe &p : parts)
            emitSubframe(frame_, p.plan, p.x, n, p.bps);
        frame_.align();
        frame_.bits(crc16(frame_.bytes.data(), frame_.bytes.size()), 16);

        const auto size = static_cast<uint32_t>(frame_.bytes.size());
        minFrame_ = minFrame_ == 0 ? size : std::min(minFrame_, size);
        maxFrame_ = std::max(maxFrame_, size);
        maxBlock_ = std::max(maxBlock_, static_cast<uint32_t>(n));
        out_.write(reinterpret_cast<const char *>(frame_.bytes.data()), static_cast<std::streamsize>(frame_.bytes.size()));
        if (!out_) {
            why = "Couldn't write " + path_;
            return false;
        }
        ++frameIndex_;
        total_ += n;

        // The signature covers the samples as they are stored: signed, little-endian, interleaved
        const size_t bytes = static_cast<size_t>(format_.bits / 8);
        size_t at = 0;
        for (size_t i = 0; i < n; ++i) {
            for (uint32_t c = 0; c < channels; ++c) {
                const auto v = static_cast<uint32_t>(block_[c][i]);
                for (size_t b = 0; b < bytes; ++b)
                    md5Bytes_[at++] = static_cast<uint8_t>(v >> (8 * b));
            }
        }
        md5_.update(md5Bytes_.data(), at);
        return true;
    }

    std::vector<std::vector<int32_t>> block_;
    std::vector<int32_t> side_;
    std::vector<int32_t> mid_;
    std::vector<uint8_t> md5Bytes_;
    BitWriter frame_;
    Md5 md5_;
    size_t fill_ = 0;
    uint64_t frameIndex_ = 0;
    uint64_t total_ = 0;
    uint32_t maxBlock_ = 0;
    uint32_t minFrame_ = 0;
    uint32_t maxFrame_ = 0;
    std::streamoff streamInfoAt_ = 0;
};

// ID3v2.3, the tag at the start of an MP3: a 10-byte header whose size is "syncsafe" (7 bits a
// byte), then frames of a 4-letter id, a 32-bit size, two flag bytes and the frame's data

uint32_t syncsafe(const uint8_t *p)
{
    return (uint32_t(p[0] & 0x7F) << 21) | (uint32_t(p[1] & 0x7F) << 14) | (uint32_t(p[2] & 0x7F) << 7) |
           uint32_t(p[3] & 0x7F);
}

void putSyncsafe(std::vector<uint8_t> &out, uint32_t value)
{
    for (int shift = 21; shift >= 0; shift -= 7)
        out.push_back(static_cast<uint8_t>((value >> shift) & 0x7F));
}

void putFrame(std::vector<uint8_t> &out, const char *id, const std::vector<uint8_t> &data)
{
    putText(out, id);
    putBytes(out, data.size(), 4, true);
    putBytes(out, 0, 2, true);
    out.insert(out.end(), data.begin(), data.end());
}

// Text as an ID3 string: Latin-1 when it's plain ASCII (encoding 0), else UTF-16 with its byte
// order mark (encoding 1). `encoding` is the byte that goes before it in the frame.
std::vector<uint8_t> id3String(const std::string &utf8, uint8_t &encoding)
{
    std::vector<uint8_t> out;
    const bool ascii = std::all_of(utf8.begin(), utf8.end(), [](char ch) { return static_cast<unsigned char>(ch) < 0x80; });
    encoding = ascii ? 0 : 1;
    if (ascii) {
        putText(out, utf8);
        return out;
    }
    out.push_back(0xFF);
    out.push_back(0xFE);
    for (size_t i = 0; i < utf8.size();) {
        // One UTF-8 sequence to a code point (a broken one becomes U+FFFD), then to UTF-16
        const auto byte = [&](size_t k) { return k < utf8.size() ? static_cast<unsigned char>(utf8[k]) : 0u; };
        const unsigned lead = byte(i);
        const int length = lead < 0x80 ? 1 : (lead >> 5) == 0x6 ? 2 : (lead >> 4) == 0xE ? 3 : (lead >> 3) == 0x1E ? 4 : 0;
        uint32_t cp = 0xFFFD;
        if (length == 1) {
            cp = lead;
        } else if (length > 1) {
            cp = lead & (0x7Fu >> length);
            for (int k = 1; k < length; ++k) {
                if ((byte(i + k) & 0xC0) != 0x80) {
                    cp = 0xFFFD;
                    break;
                }
                cp = (cp << 6) | (byte(i + k) & 0x3F);
            }
        }
        i += length > 0 ? static_cast<size_t>(length) : 1;
        if (cp >= 0x10000) {
            cp -= 0x10000;
            putBytes(out, 0xD800 + (cp >> 10), 2, false);
            putBytes(out, 0xDC00 + (cp & 0x3FF), 2, false);
        } else {
            putBytes(out, cp, 2, false);
        }
    }
    return out;
}

// A text frame (TIT2, TPE1...): its encoding, then the string
void putTextFrame(std::vector<uint8_t> &out, const char *id, const std::string &text)
{
    if (text.empty())
        return;
    uint8_t encoding = 0;
    const std::vector<uint8_t> value = id3String(text, encoding);
    std::vector<uint8_t> data{encoding};
    data.insert(data.end(), value.begin(), value.end());
    putFrame(out, id, data);
}

// A comment frame: its encoding, a language, an empty description (ended as the encoding ends a
// string) and the text
void putCommentFrame(std::vector<uint8_t> &out, const std::string &text)
{
    if (text.empty())
        return;
    uint8_t encoding = 0;
    const std::vector<uint8_t> value = id3String(text, encoding);
    std::vector<uint8_t> data{encoding};
    putText(data, "eng");
    if (encoding == 1) {
        const uint8_t empty[] = {0xFF, 0xFE, 0, 0};
        data.insert(data.end(), empty, empty + 4);
    } else {
        data.push_back(0);
    }
    data.insert(data.end(), value.begin(), value.end());
    putFrame(out, "COMM", data);
}

// The frames of the encoder's own tag worth keeping beside ours: all but those our metadata sets,
// and of its comments only the gapless information (iTunSMPB: the encoder's delay and padding)
std::vector<uint8_t> keptFrames(const std::vector<uint8_t> &frames)
{
    static const char *const ours[] = {"TIT2", "TPE1", "TALB", "TYER", "TSSE"};
    std::vector<uint8_t> kept;
    size_t at = 0;
    while (at + 10 <= frames.size() && frames[at] != 0) {
        const std::string id(reinterpret_cast<const char *>(&frames[at]), 4);
        const uint32_t size = (uint32_t(frames[at + 4]) << 24) | (uint32_t(frames[at + 5]) << 16) |
                              (uint32_t(frames[at + 6]) << 8) | uint32_t(frames[at + 7]);
        if (size > frames.size() - at - 10)
            break;
        const auto begin = frames.begin() + static_cast<std::ptrdiff_t>(at);
        const auto end = begin + 10 + static_cast<std::ptrdiff_t>(size);
        bool keep = std::none_of(std::begin(ours), std::end(ours), [&id](const char *o) { return id == o; });
        if (id == "COMM") {
            const std::string smpb = "iTunSMPB";
            keep = std::search(begin + 10, end, smpb.begin(), smpb.end()) != end;
        }
        if (keep)
            kept.insert(kept.end(), begin, end);
        at += 10 + size;
    }
    return kept;
}

// MP3 through the system's encoder (Mp3Encoder), which writes its own file beside the part file:
// on close that file's tag is merged with the metadata and its audio copied after it into the part
// file, which is then renamed as the others are
class Mp3Writer : public FileWriter {
public:
    ~Mp3Writer() override
    {
        encoder_.reset();
        removeEncoded();
    }

protected:
    bool begin(std::string &why) override
    {
        encoded_ = path_ + ".mp3part";
        encoder_ = std::make_unique<Mp3Encoder>();
        // The encoder writes mono at up to 128 kbps (as much per channel as 256 kbps stereo)
        const int kbps = format_.channels == 1 ? std::min(format_.bitrate, kMp3MonoMaxBitrate) : format_.bitrate;
        return encoder_->open(encoded_, format_.sampleRate, format_.channels, kbps, why);
    }

    bool encode(const float *in, uint64_t frames, std::string &why) override
    {
        for (uint64_t at = 0; at < frames; at += kPiece) {
            const uint64_t n = std::min(kPiece, frames - at);
            const size_t values = static_cast<size_t>(n * format_.channels);
            samples_.resize(values);
            for (size_t i = 0; i < values; ++i)
                samples_[i] = static_cast<int16_t>(toInt(in[at * format_.channels + i], 16));
            if (!encoder_->write(samples_.data(), n, why))
                return false;
        }
        return true;
    }

    bool finish(std::string &why) override
    {
        if (!encoder_->finish(why))
            return false;
        encoder_.reset();
        std::ifstream in(std::filesystem::u8path(encoded_), std::ios::binary);
        uint8_t header[10] = {};
        in.read(reinterpret_cast<char *>(header), sizeof header);
        if (in.gcount() != sizeof header) {
            why = "The MP3 encoder wrote no audio";
            return false;
        }
        // Its tag (an ID3v2.3 one without flags is the one it writes), or none: then the file is all audio
        std::vector<uint8_t> theirs;
        std::streamoff audioAt = 0;
        if (std::memcmp(header, "ID3", 3) == 0) {
            const uint32_t size = syncsafe(header + 6);
            audioAt = 10 + static_cast<std::streamoff>(size);
            if (header[3] == 3 && header[5] == 0) {
                theirs.resize(size);
                in.read(reinterpret_cast<char *>(theirs.data()), static_cast<std::streamsize>(size));
                theirs.resize(static_cast<size_t>(std::max<std::streamsize>(in.gcount(), 0)));
            }
        }

        std::vector<uint8_t> frames;
        putTextFrame(frames, "TIT2", metadata_.title);
        putTextFrame(frames, "TPE1", metadata_.artist);
        putTextFrame(frames, "TALB", metadata_.album);
        const auto digit = [](char ch) { return ch >= '0' && ch <= '9'; };
        if (metadata_.date.size() >= 4 && std::all_of(metadata_.date.begin(), metadata_.date.begin() + 4, digit))
            putTextFrame(frames, "TYER", metadata_.date.substr(0, 4));
        putTextFrame(frames, "TSSE", metadata_.software);
        putCommentFrame(frames, metadata_.comment);
        const std::vector<uint8_t> kept = keptFrames(theirs);
        frames.insert(frames.end(), kept.begin(), kept.end());
        std::vector<uint8_t> tag;
        putText(tag, "ID3");
        tag.push_back(3); // version 2.3
        tag.push_back(0);
        tag.push_back(0); // no flags
        putSyncsafe(tag, static_cast<uint32_t>(frames.size()));
        tag.insert(tag.end(), frames.begin(), frames.end());
        out_.write(reinterpret_cast<const char *>(tag.data()), static_cast<std::streamsize>(tag.size()));

        // The audio, a piece at a time
        in.clear();
        in.seekg(audioAt);
        buffer_.resize(1 << 16);
        while (in && out_) {
            in.read(reinterpret_cast<char *>(buffer_.data()), static_cast<std::streamsize>(buffer_.size()));
            out_.write(reinterpret_cast<const char *>(buffer_.data()), in.gcount());
        }
        in.close();
        removeEncoded();
        if (!out_) {
            why = "Couldn't write " + path_;
            return false;
        }
        return true;
    }

private:
    void removeEncoded()
    {
        if (encoded_.empty())
            return;
        std::error_code ec;
        std::filesystem::remove(std::filesystem::u8path(encoded_), ec);
    }

    std::unique_ptr<Mp3Encoder> encoder_;
    std::string encoded_;
    std::vector<int16_t> samples_;
};

} // namespace

std::unique_ptr<AudioWriter> makeWriter(const EncodeFormat &format)
{
    switch (format.container) {
    case EncodeFormat::Wav:
        return std::make_unique<WavWriter>();
    case EncodeFormat::Aiff:
        return std::make_unique<AiffWriter>();
    case EncodeFormat::Flac:
        return std::make_unique<FlacWriter>();
    case EncodeFormat::Mp3:
        return std::make_unique<Mp3Writer>();
    }
    return nullptr;
}

// The MD5 constants are floor(2^32 |sin(i + 1)|), computed once
namespace {

const std::array<uint32_t, 64> &md5Constants()
{
    static const std::array<uint32_t, 64> table = [] {
        std::array<uint32_t, 64> t{};
        for (int i = 0; i < 64; ++i)
            t[static_cast<size_t>(i)] = static_cast<uint32_t>(std::floor(std::fabs(std::sin(i + 1.0)) * 4294967296.0));
        return t;
    }();
    return table;
}

} // namespace

Md5::Md5()
    : state_{0x67452301u, 0xefcdab89u, 0x98badcfeu, 0x10325476u}
{
}

void Md5::transform(const uint8_t *block)
{
    static const int shifts[64] = {7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
                                   5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20,
                                   4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
                                   6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21};
    const std::array<uint32_t, 64> &k = md5Constants();
    uint32_t m[16];
    for (int i = 0; i < 16; ++i) {
        const uint8_t *p = block + 4 * i;
        m[i] = static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) |
               (static_cast<uint32_t>(p[3]) << 24);
    }
    uint32_t a = state_[0], b = state_[1], c = state_[2], d = state_[3];
    for (int i = 0; i < 64; ++i) {
        uint32_t f = 0;
        int g = 0;
        if (i < 16) {
            f = (b & c) | (~b & d);
            g = i;
        } else if (i < 32) {
            f = (d & b) | (~d & c);
            g = (5 * i + 1) % 16;
        } else if (i < 48) {
            f = b ^ c ^ d;
            g = (3 * i + 5) % 16;
        } else {
            f = c ^ (b | ~d);
            g = (7 * i) % 16;
        }
        const uint32_t sum = a + f + k[static_cast<size_t>(i)] + m[g];
        const int s = shifts[i];
        const uint32_t rotated = (sum << s) | (sum >> (32 - s));
        a = d;
        d = c;
        c = b;
        b = b + rotated;
    }
    state_[0] += a;
    state_[1] += b;
    state_[2] += c;
    state_[3] += d;
}

void Md5::put(uint8_t byte)
{
    buffer_[used_++] = byte;
    if (used_ == 64) {
        transform(buffer_);
        used_ = 0;
    }
}

void Md5::update(const void *data, size_t size)
{
    const auto *p = static_cast<const uint8_t *>(data);
    for (size_t i = 0; i < size; ++i)
        put(p[i]);
    bytes_ += size;
}

std::array<uint8_t, 16> Md5::finish()
{
    const uint64_t bits = bytes_ * 8;
    put(0x80);
    while (used_ != 56)
        put(0);
    for (int i = 0; i < 8; ++i)
        put(static_cast<uint8_t>(bits >> (8 * i)));
    std::array<uint8_t, 16> digest{};
    for (int i = 0; i < 16; ++i)
        digest[static_cast<size_t>(i)] = static_cast<uint8_t>(state_[i / 4] >> (8 * (i % 4)));
    return digest;
}

std::string md5Hex(const std::array<uint8_t, 16> &digest)
{
    static const char *hex = "0123456789abcdef";
    std::string out;
    for (uint8_t byte : digest) {
        out.push_back(hex[byte >> 4]);
        out.push_back(hex[byte & 15]);
    }
    return out;
}

namespace {

// The float temp file's header: "VBF1", channels (u32), sample rate (f64), frames (u64), all
// little-endian as the host writes them
constexpr std::streamoff kTempHeader = 24;
constexpr std::streamoff kTempFramesAt = 16;

} // namespace

FloatTempWriter::~FloatTempWriter()
{
    if (out_.is_open()) {
        out_.close();
        std::error_code ec;
        std::filesystem::remove(std::filesystem::u8path(part_), ec);
    }
}

bool FloatTempWriter::open(const std::string &path, uint32_t channels, double sampleRate, std::string &error)
{
    if (channels == 0 || channels > 8) {
        error = "Only 1 to 8 channels can be written";
        return false;
    }
    path_ = path;
    part_ = path + ".part";
    channels_ = channels;
    frames_ = 0;
    out_.open(std::filesystem::u8path(part_), std::ios::binary | std::ios::trunc);
    if (!out_) {
        error = "Couldn't create " + path;
        return false;
    }
    std::vector<uint8_t> h;
    putText(h, "VBF1");
    putBytes(h, channels, 4, false);
    uint64_t rate = 0;
    std::memcpy(&rate, &sampleRate, sizeof rate);
    putBytes(h, rate, 8, false);
    putBytes(h, 0, 8, false); // the frames, set on close
    out_.write(reinterpret_cast<const char *>(h.data()), static_cast<std::streamsize>(h.size()));
    if (!out_) {
        error = "Couldn't create " + path;
        return false;
    }
    open_ = true;
    return true;
}

bool FloatTempWriter::write(const float *interleaved, uint64_t frames)
{
    if (!open_)
        return false;
    out_.write(reinterpret_cast<const char *>(interleaved),
               static_cast<std::streamsize>(frames * channels_ * sizeof(float)));
    frames_ += frames;
    return static_cast<bool>(out_);
}

bool FloatTempWriter::close(std::string &error)
{
    if (!open_) {
        error = "The file isn't open";
        return false;
    }
    open_ = false;
    std::vector<uint8_t> count;
    putBytes(count, frames_, 8, false);
    out_.seekp(kTempFramesAt);
    out_.write(reinterpret_cast<const char *>(count.data()), static_cast<std::streamsize>(count.size()));
    out_.flush();
    const bool ok = static_cast<bool>(out_);
    out_.close();
    std::error_code ec;
    if (ok) {
        std::filesystem::remove(std::filesystem::u8path(path_), ec);
        std::filesystem::rename(std::filesystem::u8path(part_), std::filesystem::u8path(path_), ec);
    }
    if (!ok || ec) {
        std::filesystem::remove(std::filesystem::u8path(part_), ec);
        error = "Couldn't write " + path_;
        return false;
    }
    return true;
}

bool FloatTempReader::open(const std::string &path, std::string &error)
{
    in_.open(std::filesystem::u8path(path), std::ios::binary);
    unsigned char h[24] = {};
    if (!in_ || !in_.read(reinterpret_cast<char *>(h), kTempHeader) || std::memcmp(h, "VBF1", 4) != 0) {
        error = "Not a float temp file: " + path;
        return false;
    }
    auto u32 = [&h](int at) {
        return static_cast<uint32_t>(h[at]) | (static_cast<uint32_t>(h[at + 1]) << 8) |
               (static_cast<uint32_t>(h[at + 2]) << 16) | (static_cast<uint32_t>(h[at + 3]) << 24);
    };
    uint64_t rate = 0, frames = 0;
    for (int i = 7; i >= 0; --i) {
        rate = (rate << 8) | h[8 + i];
        frames = (frames << 8) | h[16 + i];
    }
    std::memcpy(&sampleRate_, &rate, sizeof sampleRate_);
    channels_ = u32(4);
    frames_ = frames;
    if (channels_ == 0 || channels_ > 8) {
        error = "Bad channel count in " + path;
        return false;
    }
    position_ = 0;
    return true;
}

uint64_t FloatTempReader::read(float *interleaved, uint64_t frames)
{
    const uint64_t n = std::min(frames, frames_ - position_);
    if (n == 0)
        return 0;
    in_.read(reinterpret_cast<char *>(interleaved), static_cast<std::streamsize>(n * channels_ * sizeof(float)));
    const auto got = static_cast<uint64_t>(in_.gcount()) / (channels_ * sizeof(float));
    position_ += got;
    return got;
}

void FloatTempReader::seek(uint64_t frame)
{
    position_ = std::min(frame, frames_);
    in_.clear();
    in_.seekg(kTempHeader + static_cast<std::streamoff>(position_ * channels_ * sizeof(float)));
}

}
