#include "ExportSelfTest.h"
#include "Harness.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <thread>
#include <iterator>
#include <limits>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "audio/AudioFile.h"
#include "audio/AudioProcessor.h"
#include "audio/Encoders.h"
#include "audio/Mastering.h"
#include "daw/io/MidiExport.h"
#include "daw/model/Project.h"
#include "daw/model/Freeze.h"
#include "audio/AudioSystem.h"
#include "audio/Midi.h"
#include "audio/Synth.h"
#include "daw/audio/AudioBridge.h"
#include "daw/audio/AudioPool.h"
#include "daw/audio/Freezer.h"
#include "daw/library/Library.h"
#include "daw/model/ProjectStore.h"
#include "daw/audio/RenderJob.h"
#include "daw/app/Settings.h"
#include "env/Daemon.h"
#include "env/Environment.h"
#include "env/Scheduler.h"

namespace tests {

namespace {

constexpr double kRate = 48000.0;
constexpr double kPi = 3.14159265358979323846;
constexpr size_t kTestFrames = 96000; // two seconds

// Each track's first clip, cut to `beats` and moved to the start: the same tracks sounding together, in a
// song `beats` long
daw::Project openingBeats(daw::Project project, double beats)
{
    for (daw::Track &track : project.tracks) {
        if (track.clips.empty())
            continue;
        daw::Clip opening = track.clips.front();
        opening.start = 0.0;
        opening.length = std::min(opening.length, beats);
        opening.notes.erase(std::remove_if(opening.notes.begin(), opening.notes.end(),
                                           [beats](const daw::Note &n) { return n.start >= beats; }),
                            opening.notes.end());
        track.clips = {opening};
    }
    return project;
}

// The song the render checks render. Each of their assertions (the pool and no pool agreeing, the stems adding
// up to the dry mix, the loudness a preset reaches, the files a preset writes) holds on any stretch of it, and
// the whole 48 s song costs ~6 s a render, so the fast tier renders the opening 8 beats of every track (4 s,
// guitar, bass and lead sounding together), on the built-in synthesizer in place of the guitar: nothing read
// from the user's sample library, so the same on every machine. VYBN_TEST_FULL renders the song as it is.
daw::Project songToRender()
{
    daw::Project song = daw::demoProject();
    if (full())
        return song;
    for (daw::Track &track : song.tracks) {
        if (track.kind == daw::TrackKind::Instrument && track.hasInstrument())
            track.instrument = audio::SynthDevice::setting();
    }
    return openingBeats(std::move(song), 8.0);
}

std::string number(const char *format, double value)
{
    char buffer[96];
    std::snprintf(buffer, sizeof buffer, format, value);
    return buffer;
}

// A sine on every channel, `amplitude` being its peak (1 is full scale)
std::vector<float> sine(size_t frames, uint32_t channels, double freq, double amplitude, double phase = 0.0)
{
    std::vector<float> out(frames * channels);
    for (size_t f = 0; f < frames; ++f) {
        const double value = amplitude * std::sin(2.0 * kPi * freq * static_cast<double>(f) / kRate + phase);
        for (uint32_t ch = 0; ch < channels; ++ch)
            out[f * channels + ch] = static_cast<float>(value);
    }
    return out;
}

// Runs a whole stream through the limiter: the output has as many frames as the input
std::vector<float> limit(const std::vector<float> &in, uint32_t channels, double ceilingDb)
{
    audio::master::TruePeakLimiter limiter(channels, kRate, ceilingDb);
    const size_t frames = in.size() / channels;
    std::vector<float> out(in.size());
    size_t written = limiter.process(in.data(), out.data(), frames);
    written += limiter.finish(out.data() + written * channels);
    out.resize(written * channels);
    return out;
}

double truePeakDb(const std::vector<float> &samples, uint32_t channels)
{
    audio::master::TruePeakMeter meter(channels);
    meter.add(samples.data(), samples.size() / channels);
    return meter.peakDb();
}

bool near(double value, double target, double tolerance)
{
    return std::fabs(value - target) <= tolerance;
}

// The encoders' own version of the integer conversion (2^(bits-1) scale, clamped to full scale)
int64_t quantised(float v, int bits)
{
    const double full = std::pow(2.0, bits - 1);
    double x = std::isnan(v) ? 0.0 : std::clamp(static_cast<double>(v), -1.0, 1.0);
    x = std::round(x * full);
    return static_cast<int64_t>(std::clamp(x, -full, full - 1.0));
}

// A 2 s stereo test signal: a 440 Hz sine at -6 dBFS on the left, a sweep from 100 Hz to 8 kHz on
// the right, with stretches at full scale and of silence
std::vector<float> testSignal(size_t frames)
{
    std::vector<float> out(frames * 2);
    double phase = 0.0;
    for (size_t f = 0; f < frames; ++f) {
        const double t = static_cast<double>(f) / kRate;
        out[f * 2] = static_cast<float>(0.5 * std::sin(2.0 * kPi * 440.0 * t));
        phase += 2.0 * kPi * (100.0 + 7900.0 * t / 2.0) / kRate;
        out[f * 2 + 1] = static_cast<float>(0.25 * std::sin(phase));
    }
    for (size_t f = 10000; f < 10500; ++f) {
        out[f * 2] = 1.0f;
        out[f * 2 + 1] = -1.0f;
    }
    for (size_t f = 20000; f < 20100; ++f)
        out[f * 2] = -1.0f;
    for (size_t f = 30000; f < 40000; ++f) {
        out[f * 2] = 0.0f;
        out[f * 2 + 1] = 0.0f;
    }
    for (size_t f = 50000; f < 50500; ++f)
        out[f * 2 + 1] = 0.0f;
    return out;
}

// Writes interleaved frames in pieces of awkward sizes, then closes the writer
bool writeInPieces(audio::AudioWriter &writer, const float *samples, size_t frames, uint32_t channels, std::string &error)
{
    static const size_t pieces[] = {1, 777, 4095, 4097, 1234, 5003};
    size_t at = 0;
    for (size_t i = 0; at < frames; ++i) {
        const size_t n = std::min(pieces[i % 6], frames - at);
        if (!writer.write(samples + at * channels, n))
            break;
        at += n;
    }
    return writer.close(error);
}

bool encodeTo(const std::string &path, const audio::EncodeFormat &format, const float *samples, size_t frames,
              std::string &error)
{
    audio::AudioMetadata meta;
    meta.title = "Encoder test title";
    meta.artist = "VYBN tests";
    meta.album = "Encoder test album";
    meta.comment = "Encoder test comment";
    meta.date = "2026-10-18";
    auto writer = audio::makeWriter(format);
    if (!writer || !writer->open(path, format, meta, error))
        return false;
    return writeInPieces(*writer, samples, frames, format.channels, error);
}

// The samples that differ from what was written: integers after the encoder's conversion, float
// exactly
size_t differing(const std::vector<float> &decoded, const std::vector<float> &original, int bits)
{
    if (decoded.size() != original.size())
        return original.size();
    size_t bad = 0;
    for (size_t i = 0; i < decoded.size(); ++i) {
        if (bits == 32)
            bad += decoded[i] != original[i] ? 1 : 0;
        else
            bad += std::llround(static_cast<double>(decoded[i]) * std::pow(2.0, bits - 1)) != quantised(original[i], bits) ? 1 : 0;
    }
    return bad;
}

std::vector<unsigned char> readFile(const std::string &path)
{
    std::ifstream in(std::filesystem::u8path(path), std::ios::binary);
    return std::vector<unsigned char>(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

bool containsText(const std::vector<unsigned char> &bytes, const std::string &text)
{
    return std::search(bytes.begin(), bytes.end(), text.begin(), text.end()) != bytes.end();
}

// Reads an AIFF file by hand (miniaudio doesn't decode AIFF): its channels, depth, rate and the
// integer samples
bool readAiff(const std::string &path, uint32_t &channels, int &bits, double &rate, std::vector<int64_t> &values)
{
    const std::vector<unsigned char> d = readFile(path);
    auto be = [&d](size_t at, size_t n) {
        uint64_t v = 0;
        for (size_t i = 0; i < n; ++i)
            v = (v << 8) | d[at + i];
        return v;
    };
    if (d.size() < 12 || std::memcmp(d.data(), "FORM", 4) != 0 || std::memcmp(d.data() + 8, "AIFF", 4) != 0)
        return false;
    uint64_t frames = 0;
    size_t at = 12;
    while (at + 8 <= d.size()) {
        const size_t size = static_cast<size_t>(be(at + 4, 4));
        if (std::memcmp(d.data() + at, "COMM", 4) == 0) {
            channels = static_cast<uint32_t>(be(at + 8, 2));
            frames = be(at + 10, 4);
            bits = static_cast<int>(be(at + 14, 2));
            const auto exponent = static_cast<int>(be(at + 16, 2) & 0x7FFF);
            rate = std::ldexp(static_cast<double>(be(at + 18, 8)), exponent - 16383 - 63);
        } else if (std::memcmp(d.data() + at, "SSND", 4) == 0) {
            const size_t bytes = static_cast<size_t>(bits / 8);
            const size_t start = at + 16 + static_cast<size_t>(be(at + 8, 4));
            if (bytes == 0 || start + frames * channels * bytes > d.size())
                return false;
            for (uint64_t i = 0; i < frames * channels; ++i) {
                const uint64_t raw = be(start + i * bytes, bytes);
                const int shift = static_cast<int>(64 - 8 * bytes);
                values.push_back(static_cast<int64_t>(raw << shift) >> shift);
            }
            return true;
        }
        at += 8 + size + (size & 1);
    }
    return false;
}

// Runs the encoder checks: each container written from the test signal and read back, the FLAC
// signature, metadata, mono, silence, refusals and the failure paths
void encoderChecks(Checker &c)
{
    namespace fs = std::filesystem;
    const fs::path base = fs::temp_directory_path() / "vybn-export-selftest";
    std::error_code ec;
    fs::remove_all(base, ec);
    fs::create_directories(base, ec);
    const std::string dir = base.u8string();
    const std::vector<float> signal = testSignal(kTestFrames);

    // The MD5 (RFC 1321) against published vectors, one of them longer than a block
    auto hashOf = [](const std::string &text) {
        audio::Md5 md;
        md.update(text.data(), text.size());
        return audio::md5Hex(md.finish());
    };
    c.check(hashOf("abc") == "900150983cd24fb0d6963f7d28e17f72", "MD5 of \"abc\" matches RFC 1321");
    c.check(hashOf("") == "d41d8cd98f00b204e9800998ecf8427e", "MD5 of nothing matches RFC 1321");
    c.check(hashOf("12345678901234567890123456789012345678901234567890123456789012345678901234567890") ==
                "57edf4a22be3c955ac49da2e2107b67a",
            "MD5 of eighty digits (longer than a block) matches RFC 1321");

    struct Case {
        const char *name;
        audio::EncodeFormat::Container container;
        int bits;
        const char *file;
    };
    const Case cases[] = {
        {"WAV 16-bit", audio::EncodeFormat::Wav, 16, "wav16.wav"},
        {"WAV 24-bit", audio::EncodeFormat::Wav, 24, "wav24.wav"},
        {"WAV 32-bit float", audio::EncodeFormat::Wav, 32, "wav32f.wav"},
        {"AIFF 16-bit", audio::EncodeFormat::Aiff, 16, "aiff16.aiff"},
        {"AIFF 24-bit", audio::EncodeFormat::Aiff, 24, "aiff24.aiff"},
        {"FLAC 16-bit", audio::EncodeFormat::Flac, 16, "flac16.flac"},
        {"FLAC 24-bit", audio::EncodeFormat::Flac, 24, "flac24.flac"},
    };
    uint64_t byteSize[7] = {};
    bool metadataFound = true;
    for (size_t k = 0; k < std::size(cases); ++k) {
        const Case &kase = cases[k];
        audio::EncodeFormat format;
        format.container = kase.container;
        format.bits = kase.bits;
        format.sampleRate = 48000;
        format.channels = 2;
        const std::string path = dir + "/" + kase.file;
        std::string error;
        const bool written = encodeTo(path, format, signal.data(), kTestFrames, error);
        const bool noPart = !fs::exists(std::filesystem::u8path(path + ".part"), ec);
        const std::vector<unsigned char> bytes = written ? readFile(path) : std::vector<unsigned char>();
        byteSize[k] = bytes.size();
        if (written && !(containsText(bytes, "Encoder test title") && containsText(bytes, "Encoder test album")))
            metadataFound = false;

        size_t bad = signal.size();
        std::string detail = error;
        if (written && kase.container == audio::EncodeFormat::Aiff) {
            uint32_t ch = 0;
            int bitsRead = 0;
            double rate = 0.0;
            std::vector<int64_t> values;
            if (readAiff(path, ch, bitsRead, rate, values) && ch == 2 && bitsRead == kase.bits && std::llround(rate) == 48000) {
                bad = 0;
                for (size_t i = 0; i < values.size() && i < signal.size(); ++i)
                    bad += values[i] != quantised(signal[i], kase.bits) ? 1 : 0;
                if (values.size() != signal.size())
                    bad = signal.size();
            } else {
                detail = "AIFF header not read back";
            }
        } else if (written) {
            std::shared_ptr<audio::AudioData> data = audio::loadAudioFile(path, error);
            if (data && data->channels == 2 && data->frames == kTestFrames && std::llround(data->sampleRate) == 48000) {
                bad = differing(data->samples, signal, kase.bits);
            } else {
                detail = data ? "decoded with the wrong layout" : error;
            }
        }
        c.check(written && noPart && bad == 0, std::string(kase.name) + " decodes back sample-exact",
                bad == 0 ? "" : (detail.empty() ? "(" + std::to_string(bad) + " samples differ)" : detail));
    }

    // FLAC's STREAMINFO signature is the MD5 of the samples as stored, and FLAC is smaller than WAV
    for (size_t k : {size_t(5), size_t(6)}) {
        const int bits = cases[k].bits;
        std::vector<unsigned char> raw;
        for (size_t i = 0; i < signal.size(); ++i) {
            const auto q = static_cast<uint32_t>(quantised(signal[i], bits));
            for (int b = 0; b < bits / 8; ++b)
                raw.push_back(static_cast<unsigned char>(q >> (8 * b)));
        }
        audio::Md5 md;
        md.update(raw.data(), raw.size());
        const std::array<uint8_t, 16> digest = md.finish();
        const std::vector<unsigned char> file = readFile(dir + "/" + cases[k].file);
        const bool signedOk = file.size() > 42 && std::memcmp(file.data() + 26, digest.data(), 16) == 0;
        c.check(signedOk, std::string(cases[k].name) + " STREAMINFO carries the MD5 of its samples");
    }
    {
        const double ratio16 = 100.0 * static_cast<double>(byteSize[5]) / static_cast<double>(byteSize[0]);
        const double ratio24 = 100.0 * static_cast<double>(byteSize[6]) / static_cast<double>(byteSize[1]);
        c.check(byteSize[5] < byteSize[0] && byteSize[6] < byteSize[1], "FLAC files are smaller than the WAV of the same depth",
                "(16-bit " + number("%.1f%%", ratio16) + " of WAV, 24-bit " + number("%.1f%%", ratio24) + ")");
    }
    c.check(metadataFound, "The title and album are in the bytes of every container");

    // A mono FLAC of the left channel
    {
        std::vector<float> mono(kTestFrames);
        for (size_t i = 0; i < kTestFrames; ++i)
            mono[i] = signal[i * 2];
        audio::EncodeFormat format;
        format.container = audio::EncodeFormat::Flac;
        format.bits = 16;
        format.channels = 1;
        std::string error;
        const std::string path = dir + "/mono16.flac";
        const bool written = encodeTo(path, format, mono.data(), kTestFrames, error);
        std::shared_ptr<audio::AudioData> data = written ? audio::loadAudioFile(path, error) : nullptr;
        const size_t bad = data && data->channels == 1 ? differing(data->samples, mono, 16) : mono.size();
        c.check(written && bad == 0, "A mono FLAC decodes back sample-exact", bad ? error : std::string());
    }

    // Silence is coded as CONSTANT subframes, so it takes almost nothing
    {
        const std::vector<float> silence(kTestFrames * 2, 0.0f);
        audio::EncodeFormat format;
        format.container = audio::EncodeFormat::Flac;
        format.bits = 16;
        std::string error;
        const std::string path = dir + "/silence.flac";
        const bool written = encodeTo(path, format, silence.data(), kTestFrames, error);
        const uint64_t size = written ? readFile(path).size() : 0;
        std::shared_ptr<audio::AudioData> data = written ? audio::loadAudioFile(path, error) : nullptr;
        const bool zeros = data && differing(data->samples, silence, 16) == 0;
        c.check(written && size < 1000 && zeros, "Two seconds of silence in FLAC takes under 1 kB", number("(%.0f bytes)", static_cast<double>(size)));
    }

    // Formats the containers can't hold are refused, with a reason, and leave nothing behind
    for (audio::EncodeFormat::Container container : {audio::EncodeFormat::Flac, audio::EncodeFormat::Aiff}) {
        audio::EncodeFormat format;
        format.container = container;
        format.bits = 32;
        const std::string path = dir + (container == audio::EncodeFormat::Flac ? "/refused.flac" : "/refused.aiff");
        std::string error;
        auto writer = audio::makeWriter(format);
        const bool opened = writer && writer->open(path, format, audio::AudioMetadata(), error);
        c.check(!opened && !error.empty() && !fs::exists(path, ec) && !fs::exists(path + ".part", ec),
                container == audio::EncodeFormat::Flac ? "FLAC refuses 32-bit, with a reason"
                                                       : "AIFF refuses 32-bit, with a reason",
                error);
    }

    // MP3, through the system's encoder. Lossy, so what comes back is compared by its error against
    // the signal, at the delay the encoder and decoder add (about 1200 frames), one channel at a time.
    {
        // How far below the signal the error is, in dB, over [from, to) of one channel at `delay`
        auto snrAt = [](const std::vector<float> &decoded, const std::vector<float> &original, uint32_t channels,
                        int channel, size_t delay, size_t from, size_t to) {
            const size_t decodedFrames = decoded.size() / channels;
            double signal = 0.0, noise = 0.0;
            for (size_t f = from; f < to && f + delay < decodedFrames; ++f) {
                for (uint32_t ch = 0; ch < channels; ++ch) {
                    if (channel >= 0 && ch != static_cast<uint32_t>(channel))
                        continue;
                    const double want = original[f * channels + ch];
                    const double got = decoded[(f + delay) * channels + ch];
                    signal += want * want;
                    noise += (got - want) * (got - want);
                }
            }
            return 10.0 * std::log10(signal / std::max(noise, 1e-20));
        };
        // The delay, from every channel across the silence ending at frame 40000: the 440 Hz sine
        // repeats every 1200 frames, so a window of it alone would match one period early or late
        auto delayOf = [&snrAt](const std::vector<float> &decoded, const std::vector<float> &original, uint32_t channels) {
            size_t delay = 0;
            double best = -1e9;
            for (size_t d = 0; d <= 4000; ++d) {
                const double snr = snrAt(decoded, original, channels, -1, d, 39000, 41500);
                if (snr > best) {
                    best = snr;
                    delay = d;
                }
            }
            return delay;
        };
        // Each channel is then measured over frames 41000 to 49500, where neither jumps to full scale
        // or silence (MP3 smears those, before and after)
        auto snrOf = [&](const std::vector<float> &decoded, uint32_t channels, const std::vector<float> &original,
                         uint32_t channel, size_t &delay) {
            delay = delayOf(decoded, original, channels);
            return snrAt(decoded, original, channels, static_cast<int>(channel), delay, 41000, 49500);
        };

        audio::EncodeFormat format;
        format.container = audio::EncodeFormat::Mp3;
        format.bits = 16;
        format.sampleRate = 48000;
        format.bitrate = 320;
        std::string error;
        const std::string path = dir + "/stereo320.mp3";
        const bool written = encodeTo(path, format, signal.data(), kTestFrames, error);
        const bool leftovers = fs::exists(fs::u8path(path + ".part"), ec) || fs::exists(fs::u8path(path + ".mp3part"), ec);
        const std::vector<unsigned char> bytes = written ? readFile(path) : std::vector<unsigned char>();
        std::shared_ptr<audio::AudioData> data = written ? audio::loadAudioFile(path, error) : nullptr;
        const bool layout = data && data->channels == 2 && std::llround(data->sampleRate) == 48000 &&
                            data->frames >= kTestFrames && data->frames <= kTestFrames + 4000;
        c.check(written && !leftovers && layout, "An MP3 is written and decodes as 48 kHz stereo of about the same length",
                data ? number("(%.0f frames)", static_cast<double>(data->frames)) : error);
        size_t delayLeft = 0, delayRight = 0;
        const double left = layout ? snrOf(data->samples, 2, signal, 0, delayLeft) : 0.0;
        const double right = layout ? snrOf(data->samples, 2, signal, 1, delayRight) : 0.0;
        c.check(left > 40.0 && right > 40.0 && delayLeft == delayRight,
                "At 320 kbps both channels come back close to the signal, in their places",
                number("(left %.1f dB, ", left) + number("right %.1f dB, ", right) +
                    number("delay %.0f frames)", static_cast<double>(delayLeft)));

        // The tag: ID3v2.3 first, with the metadata, and the encoder's gapless information kept
        c.check(bytes.size() > 10 && std::memcmp(bytes.data(), "ID3\x03", 4) == 0 && containsText(bytes, "TIT2") &&
                    containsText(bytes, "Encoder test title") && containsText(bytes, "Encoder test album") &&
                    containsText(bytes, "VYBN tests") && containsText(bytes, "Encoder test comment") &&
                    containsText(bytes, "TYER") && containsText(bytes, "2026"),
                "An MP3's ID3 tag holds the title, artist, album, comment and year");
        c.check(containsText(bytes, "iTunSMPB"), "An MP3 keeps the encoder's gapless information (iTunSMPB)");

        // The bitrate sets the size: 128 kbps is about 128000 / 8 bytes a second
        format.bitrate = 128;
        const std::string small = dir + "/stereo128.mp3";
        const bool smallWritten = encodeTo(small, format, signal.data(), kTestFrames, error);
        const double seconds = static_cast<double>(kTestFrames) / 48000.0;
        const double kbps = smallWritten ? static_cast<double>(readFile(small).size()) * 8.0 / seconds / 1000.0 : 0.0;
        c.check(smallWritten && kbps > 120.0 && kbps < 140.0 && readFile(small).size() < bytes.size(),
                "An MP3 at 128 kbps is that size, and smaller than at 320", number("(%.1f kbps)", kbps));

        // Mono at 44.1 kHz, and a title that isn't ASCII (written as UTF-16)
        std::vector<float> mono(kTestFrames);
        for (size_t i = 0; i < kTestFrames; ++i)
            mono[i] = signal[i * 2];
        audio::EncodeFormat monoFormat = format;
        monoFormat.channels = 1;
        monoFormat.sampleRate = 44100;
        monoFormat.bitrate = 192;
        const std::string monoPath = dir + "/mono.mp3";
        audio::AudioMetadata meta;
        meta.title = "Caf\xC3\xA9 \xE2\x99\xAA";
        auto writer = audio::makeWriter(monoFormat);
        const bool monoWritten = writer && writer->open(monoPath, monoFormat, meta, error) &&
                                 writeInPieces(*writer, mono.data(), kTestFrames, 1, error);
        std::shared_ptr<audio::AudioData> monoData = monoWritten ? audio::loadAudioFile(monoPath, error) : nullptr;
        size_t monoDelay = 0;
        const double monoSnr = monoData && monoData->channels == 1 ? snrOf(monoData->samples, 1, mono, 0, monoDelay) : 0.0;
        c.check(monoData && monoData->channels == 1 && std::llround(monoData->sampleRate) == 44100 && monoSnr > 30.0,
                "A mono MP3 at 44.1 kHz decodes as mono, close to the signal",
                monoData ? number("(%.1f dB, ", monoSnr) + number("%.0f channels)", monoData->channels) : error);
        const std::vector<unsigned char> monoBytes = monoWritten ? readFile(monoPath) : std::vector<unsigned char>();
        const unsigned char utf16[] = {0xFF, 0xFE, 'C', 0, 'a', 0, 'f', 0, 0xE9, 0, ' ', 0, 0x6A, 0x26};
        const bool titled = std::search(monoBytes.begin(), monoBytes.end(), std::begin(utf16), std::end(utf16)) != monoBytes.end();
        c.check(titled, "A title that isn't ASCII is written as UTF-16");

        // What MP3 can't hold is refused with a reason before anything is written
        struct Refused {
            const char *what;
            uint32_t rate;
            int bitrate;
            int bits;
            uint32_t channels;
        };
        const Refused refusals[] = {
            {"96 kHz", 96000, 320, 16, 2}, {"300 kbps", 48000, 300, 16, 2}, {"24-bit", 48000, 320, 24, 2}, {"6 channels", 48000, 320, 16, 6}};
        for (const Refused &r : refusals) {
            audio::EncodeFormat bad = format;
            bad.sampleRate = r.rate;
            bad.bitrate = r.bitrate;
            bad.bits = r.bits;
            bad.channels = r.channels;
            const std::string badPath = dir + "/refused.mp3";
            std::string why;
            auto badWriter = audio::makeWriter(bad);
            const bool opened = badWriter && badWriter->open(badPath, bad, audio::AudioMetadata(), why);
            badWriter.reset();
            c.check(!opened && !why.empty() && !fs::exists(fs::u8path(badPath), ec) &&
                        !fs::exists(fs::u8path(badPath + ".part"), ec) && !fs::exists(fs::u8path(badPath + ".mp3part"), ec),
                    std::string("MP3 refuses ") + r.what + ", with a reason", why);
        }

        // A writer dropped before it's closed (a cancelled render) leaves nothing behind
        {
            const std::string dropped = dir + "/dropped.mp3";
            auto dropWriter = audio::makeWriter(format);
            std::string why;
            const bool opened = dropWriter && dropWriter->open(dropped, format, audio::AudioMetadata(), why) &&
                                dropWriter->write(signal.data(), 10000);
            dropWriter.reset();
            c.check(opened && !fs::exists(fs::u8path(dropped), ec) && !fs::exists(fs::u8path(dropped + ".part"), ec) &&
                        !fs::exists(fs::u8path(dropped + ".mp3part"), ec),
                    "An MP3 dropped before closing leaves no files", why);
        }
    }

    // A folder that doesn't exist: refused with a reason, no part file
    {
        audio::EncodeFormat format;
        const std::string path = dir + "/no such folder/out.wav";
        std::string error;
        auto writer = audio::makeWriter(format);
        const bool opened = writer->open(path, format, audio::AudioMetadata(), error);
        c.check(!opened && !error.empty() && !fs::exists(path + ".part", ec), "A missing folder fails with a reason and leaves no part file", error);
    }

    // A file already there is replaced by the finished one
    {
        const std::string path = dir + "/replaced.wav";
        {
            std::ofstream old(std::filesystem::u8path(path), std::ios::binary);
            old << "old contents";
        }
        audio::EncodeFormat format;
        std::string error;
        const bool written = encodeTo(path, format, signal.data(), 1000, error);
        const std::vector<unsigned char> bytes = readFile(path);
        c.check(written && bytes.size() > 1000 && std::memcmp(bytes.data(), "RIFF", 4) == 0, "A finished file replaces the one already there", error);
    }

    // Float temp files: written in pieces, read back exactly, and seekable
    {
        audio::FloatTempWriter writer;
        std::string error;
        const std::string path = dir + "/render.vbf";
        bool ok = writer.open(path, 2, kRate, error);
        for (size_t at = 0; ok && at < kTestFrames;) {
            const size_t n = std::min<size_t>(3001, kTestFrames - at);
            ok = writer.write(signal.data() + at * 2, n);
            at += n;
        }
        ok = ok && writer.close(error);
        audio::FloatTempReader reader;
        ok = ok && reader.open(path, error) && reader.channels() == 2 && reader.sampleRate() == kRate && reader.frames() == kTestFrames;
        std::vector<float> back;
        std::vector<float> piece(4099 * 2);
        for (uint64_t n = reader.read(piece.data(), 4099); n > 0; n = reader.read(piece.data(), 4099))
            back.insert(back.end(), piece.begin(), piece.begin() + static_cast<std::ptrdiff_t>(n * 2));
        ok = ok && back == signal;
        reader.seek(50000);
        float tail[20] = {};
        const uint64_t got = reader.read(tail, 10);
        ok = ok && got == 10 && std::memcmp(tail, signal.data() + 100000, sizeof tail) == 0;
        c.check(ok && !fs::exists(std::filesystem::u8path(path + ".part"), ec), "Float temp files read back exactly, and seek", error);
    }

    fs::remove_all(base, ec);
}

// A Standard MIDI File read back for the checks: channel messages keep their status byte (with
// the channel) and data bytes, meta events have status 0xFF and their type. The writer never uses
// running status, so this reader refuses it.
struct MidiEvent {
    long long tick = 0;
    int status = 0;
    int meta = -1;
    std::vector<int> data;
};

struct MidiFile {
    int format = -1;
    int division = -1;
    std::vector<std::vector<MidiEvent>> tracks; // the conductor first
};

bool parseMidi(const std::vector<unsigned char> &bytes, MidiFile &out, std::string &why)
{
    if (bytes.size() < 14 || std::memcmp(bytes.data(), "MThd", 4) != 0) {
        why = "No MThd header";
        return false;
    }
    const auto u16 = [&](size_t i) { return static_cast<int>((bytes[i] << 8) | bytes[i + 1]); };
    const auto u32 = [&](size_t i) {
        return (static_cast<size_t>(bytes[i]) << 24) | (static_cast<size_t>(bytes[i + 1]) << 16) |
               (static_cast<size_t>(bytes[i + 2]) << 8) | static_cast<size_t>(bytes[i + 3]);
    };
    if (u32(4) != 6) {
        why = "Header length isn't 6";
        return false;
    }
    out.format = u16(8);
    const int tracks = u16(10);
    out.division = u16(12);
    size_t p = 14;
    for (int t = 0; t < tracks; ++t) {
        if (p + 8 > bytes.size() || std::memcmp(&bytes[p], "MTrk", 4) != 0) {
            why = "Track " + std::to_string(t) + " has no MTrk header";
            return false;
        }
        const size_t length = u32(p + 4);
        p += 8;
        if (length > bytes.size() - p) {
            why = "Track " + std::to_string(t) + " runs past the file";
            return false;
        }
        const size_t end = p + length;
        // Reads a variable-length quantity, or fails at the end of the track
        const auto vlq = [&](uint32_t &value) {
            value = 0;
            for (int groups = 0; groups < 4; ++groups) {
                if (p >= end)
                    return false;
                const unsigned char b = bytes[p++];
                value = (value << 7) | (b & 0x7F);
                if (!(b & 0x80))
                    return true;
            }
            return false;
        };
        long long tick = 0;
        std::vector<MidiEvent> events;
        while (p < end) {
            uint32_t delta = 0;
            if (!vlq(delta)) {
                why = "Bad delta time in track " + std::to_string(t);
                return false;
            }
            tick += delta;
            if (p >= end) {
                why = "Track " + std::to_string(t) + " ends inside an event";
                return false;
            }
            MidiEvent event;
            event.tick = tick;
            event.status = bytes[p++];
            if (event.status == 0xFF) {
                uint32_t len = 0;
                if (p >= end) {
                    why = "Bad meta event in track " + std::to_string(t);
                    return false;
                }
                event.meta = bytes[p++];
                if (!vlq(len) || len > end - p) {
                    why = "Bad meta event in track " + std::to_string(t);
                    return false;
                }
                event.data.assign(bytes.begin() + static_cast<long>(p), bytes.begin() + static_cast<long>(p + len));
                p += len;
            } else if (event.status < 0x80 || event.status >= 0xF0) {
                why = "Unexpected status " + std::to_string(event.status) + " in track " + std::to_string(t);
                return false;
            } else {
                const size_t count = (event.status & 0xF0) == 0xC0 || (event.status & 0xF0) == 0xD0 ? 1 : 2;
                if (count > end - p) {
                    why = "Channel message runs past its track";
                    return false;
                }
                for (size_t k = 0; k < count; ++k)
                    event.data.push_back(bytes[p++]);
            }
            events.push_back(event);
        }
        out.tracks.push_back(events);
    }
    if (p != bytes.size()) {
        why = "Bytes after the last track";
        return false;
    }
    return true;
}

// The channel messages of one kind (0x90 note on...) on one channel, in the track's order
std::vector<MidiEvent> messages(const std::vector<MidiEvent> &events, int kind, int channel)
{
    std::vector<MidiEvent> out;
    for (const MidiEvent &e : events)
        if (e.meta < 0 && e.status == (kind | channel))
            out.push_back(e);
    return out;
}

// Note-ons of a channel with their keys and ticks, and the tick each key's note off came at
// (the first off of that key after its on)
struct PlayedNote {
    long long on = 0, off = -1;
    int key = 0, velocity = 0;
};

std::vector<PlayedNote> playedNotes(const std::vector<MidiEvent> &events, int channel)
{
    std::vector<PlayedNote> notes;
    for (const MidiEvent &e : messages(events, 0x90, channel)) {
        if (e.data[1] == 0)
            continue;
        notes.push_back({e.tick, -1, e.data[0], e.data[1]});
    }
    for (const MidiEvent &e : events) {
        if (e.meta >= 0 || (e.status & 0xF0) != 0x80 || (e.status & 0x0F) != channel)
            continue;
        for (PlayedNote &note : notes)
            if (note.off < 0 && note.key == e.data[0] && note.on <= e.tick)
                note.off = e.tick;
    }
    return notes;
}

std::string metaText(const MidiEvent &e) { return std::string(e.data.begin(), e.data.end()); }

// The 14-bit value of a pitch bend message
int bendValue(const MidiEvent &e) { return e.data[0] | (e.data[1] << 7); }

void midiChecks(Checker &c)
{
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path base = fs::temp_directory_path(ec) / "vybn-midi-selftest";
    fs::create_directories(base, ec);
    const std::string path = base.u8string() + "/export.mid";

    // A project with an instrument track (a muted note, a note past its clip's end, a note running
    // past it, a mod wheel ramp and a pitch bend lane), an audio track and a second instrument track
    daw::Project project;
    project.name = "Ramp Test";
    project.timing = audio::TempoMap(100.0, 3);
    project.tracks.clear();
    daw::Track lead;
    lead.name = "Lead";
    lead.kind = daw::TrackKind::Instrument;
    daw::Clip leadClip("Lead A", 0.0, 8.0, {
        {0.0, 1.0, 60, 0.8f, false, false},
        {1.0, 2.0, 64, 0.8f, false, false},
        {2.0, 1.0, 67, 0.8f, true, false},  // muted
        {7.5, 2.0, 65, 0.8f, false, false}, // runs past the clip's end: cut there
        {9.0, 1.0, 69, 0.8f, false, false}, // past the clip's end: doesn't play
    });
    daw::ControlLane wheel;
    wheel.controller = 1;
    wheel.points = {{0.0, 0.0f}, {4.0, 1.0f}};
    daw::ControlLane bend;
    bend.controller = daw::kPitchBend;
    bend.points = {{0.0, 0.5f}, {3.0, 0.75f}};
    leadClip.controls = {wheel, bend};
    lead.clips.push_back(leadClip);

    daw::Track audio;
    audio.name = "Audio";
    audio.kind = daw::TrackKind::Audio;
    daw::Clip take("Take", 0.0, 4.0);
    take.audioFile = "take.wav";
    audio.clips.push_back(take);

    daw::Track bass;
    bass.name = "Bass";
    bass.kind = daw::TrackKind::Instrument;
    bass.clips.emplace_back("Bass A", 0.0, 4.0, std::vector<daw::Note>{{0.0, 4.0, 40, 0.5f, false, false}});
    project.tracks = {lead, audio, bass};

    // The whole project: conductor, then the two instrument tracks on channels 0 and 1
    std::string error;
    daw::MidiExportOptions all;
    const bool written = daw::writeMidiFile(project, all, path, error);
    std::vector<unsigned char> bytes;
    {
        std::ifstream in(fs::u8path(path), std::ios::binary);
        bytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    MidiFile file;
    std::string why;
    const bool parsed = written && parseMidi(bytes, file, why);
    c.check(written && parsed, "A MIDI file is written and reads back", written ? why : error);
    const bool noPart = !fs::exists(fs::u8path(path + ".part"), ec);
    c.check(noPart, "No part file is left beside a MIDI file");
    if (!parsed) {
        std::printf("FAIL  %s\n", why.c_str());
        return;
    }
    c.check(file.format == 1 && file.division == 480 && file.tracks.size() == 3,
            "The file is format 1 at 480 ticks a beat with three tracks",
            number("(%.0f tracks)", static_cast<double>(file.tracks.size())));

    const std::vector<MidiEvent> &conductor = file.tracks[0];
    bool tempoOk = false, signatureOk = false, nameOk = false;
    for (const MidiEvent &e : conductor) {
        if (e.meta == 0x51 && e.data.size() == 3)
            tempoOk = e.tick == 0 && ((e.data[0] << 16) | (e.data[1] << 8) | e.data[2]) == 600000;
        if (e.meta == 0x58 && e.data.size() == 4)
            signatureOk = e.tick == 0 && e.data[0] == 3 && e.data[1] == 2 && e.data[2] == 24 && e.data[3] == 8;
        if (e.meta == 0x03)
            nameOk = metaText(e) == "Ramp Test";
    }
    c.check(tempoOk, "The conductor sets 100 BPM (600000 microseconds a quarter) at tick 0");
    c.check(signatureOk, "The conductor's time signature is 3/4");
    c.check(nameOk, "The conductor is named after the project");
    c.check(file.tracks.size() == 3 && metaText(file.tracks[1][0]) == "Lead" && metaText(file.tracks[2][0]) == "Bass",
            "The instrument tracks are named and the audio track is left out");

    // Notes: the muted one and the one past the clip's end are missing; the cut note ends at the clip's end
    const std::vector<PlayedNote> leadNotes = playedNotes(file.tracks[1], 0);
    c.check(leadNotes.size() == 3, "The lead writes three notes (muted and past-the-end ones left out)",
            number("(%.0f notes)", static_cast<double>(leadNotes.size())));
    if (leadNotes.size() == 3) {
        c.check(leadNotes[0].key == 60 && leadNotes[0].on == 0 && leadNotes[0].off == 480,
                "The first note starts at tick 0 and lasts a beat (480 ticks)");
        c.check(leadNotes[1].key == 64 && leadNotes[1].on == 480 && leadNotes[1].off == 1440,
                "The second note starts at beat 1 and lasts two beats");
        c.check(leadNotes[2].key == 65 && leadNotes[2].on == 3600 && leadNotes[2].off == 3840,
                "The note running past the clip's end is cut at beat 8 (240 ticks)");
        c.check(leadNotes[0].velocity == 102, "Velocity 0.8 is written as 102 of 127",
                number("(%.0f)", static_cast<double>(leadNotes[0].velocity)));
    }
    bool offsSixtyFour = true;
    for (const MidiEvent &e : file.tracks[1])
        if (e.meta < 0 && (e.status & 0xF0) == 0x80)
            offsSixtyFour = offsSixtyFour && e.data[1] == 64;
    c.check(offsSixtyFour, "Note offs carry velocity 64");

    // The mod wheel ramps from 0 to 127 over four beats, its points at 0 and beat 4
    std::vector<int> wheelValues;
    long long wheelLast = -1;
    bool monotone = true;
    for (const MidiEvent &e : messages(file.tracks[1], 0xB0, 0)) {
        if (e.data[0] != 1)
            continue;
        monotone = monotone && e.tick >= wheelLast;
        wheelLast = e.tick;
        wheelValues.push_back(e.data[1]);
    }
    c.check(!wheelValues.empty() && wheelValues.front() == 0 && wheelValues.back() == 127,
            "The mod wheel ramp starts at 0 and ends at 127", number("(%.0f changes)", static_cast<double>(wheelValues.size())));
    c.check(monotone && wheelValues.size() >= 100, "The mod wheel ramp rises in time, a value each time it changes");
    bool wheelEnd = false;
    for (const MidiEvent &e : messages(file.tracks[1], 0xB0, 0))
        wheelEnd = wheelEnd || (e.data[0] == 1 && e.data[1] == 127 && e.tick == 1920);
    c.check(wheelEnd, "The mod wheel's last point (beat 4) lands on tick 1920");

    // The pitch bend: centre at the start, 0.75 (12288) at beat 3
    bool centre = false, raised = false;
    for (const MidiEvent &e : messages(file.tracks[1], 0xE0, 0)) {
        centre = centre || (e.tick == 0 && bendValue(e) == 8192);
        raised = raised || (e.tick == 1440 && bendValue(e) == 12288);
    }
    c.check(centre, "The pitch bend starts at its centre (8192) on tick 0");
    c.check(raised, "The pitch bend at beat 3 is 12288 (0.75) on tick 1440");

    // The second instrument track plays on channel 1
    const std::vector<PlayedNote> bassNotes = playedNotes(file.tracks[2], 1);
    c.check(bassNotes.size() == 1 && bassNotes[0].key == 40 && bassNotes[0].on == 0 && bassNotes[0].off == 1920,
            "The bass plays key 40 on channel 1 for four beats");
    c.check(messages(file.tracks[2], 0x90, 0).empty() && messages(file.tracks[1], 0x90, 1).empty(),
            "Each track sticks to its own channel");

    // A range: beats 1 to 5, only the lead. Time 0 is beat 1, so the note at beat 1 starts at tick 0
    // and the note at beat 7.5 isn't there; the mod wheel's last point (beat 4) is at tick 1440.
    daw::MidiExportOptions range;
    range.tracks = {0};
    range.fromBeat = 1.0;
    range.toBeat = 5.0;
    const bool rangeWritten = daw::writeMidiFile(project, range, path, error);
    bytes.clear();
    {
        std::ifstream in(fs::u8path(path), std::ios::binary);
        bytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    MidiFile ranged;
    const bool rangedParsed = rangeWritten && parseMidi(bytes, ranged, why);
    c.check(rangedParsed && ranged.tracks.size() == 2, "A range writes the conductor and the chosen track", rangeWritten ? why : error);
    if (rangedParsed && ranged.tracks.size() == 2) {
        const std::vector<PlayedNote> cut = playedNotes(ranged.tracks[1], 0);
        c.check(cut.size() == 1 && cut[0].key == 64 && cut[0].on == 0 && cut[0].off == 960,
                "A range shifts its notes to start at its first beat, and keeps the one running on from it",
                number("(%.0f notes)", static_cast<double>(cut.size())));
        long long highest = 0;
        for (const MidiEvent &e : ranged.tracks[1])
            highest = std::max(highest, e.tick);
        c.check(highest <= 1920, "Nothing is written past the range's end", number("(last tick %.0f)", static_cast<double>(highest)));
        bool wheelAt = false;
        for (const MidiEvent &e : messages(ranged.tracks[1], 0xB0, 0))
            wheelAt = wheelAt || (e.data[0] == 1 && e.data[1] == 127 && e.tick == 1440);
        c.check(wheelAt, "Controllers shift with the range (beat 4 is tick 1440 from beat 1)");
    }

    // A note running past the range's end is cut at it
    daw::MidiExportOptions cutEnd;
    cutEnd.tracks = {0};
    cutEnd.toBeat = 1.5;
    const bool cutWritten = daw::writeMidiFile(project, cutEnd, path, error);
    bytes.clear();
    {
        std::ifstream in(fs::u8path(path), std::ios::binary);
        bytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    MidiFile cutFile;
    const bool cutParsed = cutWritten && parseMidi(bytes, cutFile, why);
    if (cutParsed && cutFile.tracks.size() == 2) {
        const std::vector<PlayedNote> ends = playedNotes(cutFile.tracks[1], 0);
        c.check(ends.size() == 2 && ends[1].key == 64 && ends[1].on == 480 && ends[1].off == 720,
                "A note running past the range's end is cut at it (beat 1.5)");
    } else {
        c.check(false, "A note running past the range's end is cut at it (beat 1.5)", cutWritten ? why : error);
    }

    // The drum channel: the first track plays on channel 9
    daw::MidiExportOptions drums;
    drums.drumChannel = true;
    const bool drumWritten = daw::writeMidiFile(project, drums, path, error);
    bytes.clear();
    {
        std::ifstream in(fs::u8path(path), std::ios::binary);
        bytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    MidiFile drumFile;
    const bool drumParsed = drumWritten && parseMidi(bytes, drumFile, why);
    c.check(drumParsed && drumFile.tracks.size() == 3 && playedNotes(drumFile.tracks[1], 9).size() == 3 &&
                playedNotes(drumFile.tracks[2], 0).size() == 1,
            "With the drum channel the first track plays on channel 9 and the next on 0", drumWritten ? why : error);

    // A folder that doesn't exist is refused, with a reason, and nothing is left behind
    const std::string missing = base.u8string() + "/no-such-folder/export.mid";
    const bool refused = !daw::writeMidiFile(project, all, missing, error) && !error.empty() && !fs::exists(fs::u8path(missing + ".part"), ec);
    c.check(refused, "A MIDI file that can't be created is refused with a reason", error);
    fs::remove_all(base, ec);
}

// A daemon that runs a step on each loop iteration of its environment, and stops the environment
// when the step returns true (the render checks run on the environment's loop, so the library's
// loads, which are posted to it, get delivered)
class StepDaemon : public env::Daemon {
public:
    StepDaemon(env::Environment &, std::function<bool(env::Environment &)> step)
        : env::Daemon("Render checks"), step_(std::move(step))
    {
    }

    void tick(env::Environment &environment) override
    {
        if (step_(environment))
            environment.requestStop();
    }

private:
    std::function<bool(env::Environment &)> step_;
};

// Reads a temp file of the render back as interleaved stereo
bool loadRender(const std::string &path, std::vector<float> &samples)
{
    audio::FloatTempReader reader;
    std::string error;
    if (!reader.open(path, error))
        return false;
    samples.resize(static_cast<size_t>(reader.frames()) * reader.channels());
    reader.read(samples.data(), reader.frames());
    return true;
}

// The offline engine (daw::RenderPass) against itself: the demo's master rendered by an engine
// that mixes on a pool of its own and by one with no pool (the same graph, mixed on the render
// thread) must match within 1e-6. The stems of every track must add up to the dry mix, a cancelled
// pass must stop at once and leave nothing behind, and the speed of a pass is reported. Uses the
// user's sample library (read only), so the demo's guitars play when it is installed.
void renderChecks(Checker &c)
{
    using namespace daw;

    const std::string dir = (std::filesystem::temp_directory_path() / "vybn-render-selftest").u8string();
    std::error_code ec;
    std::filesystem::remove_all(std::filesystem::u8path(dir), ec);
    std::filesystem::create_directories(std::filesystem::u8path(dir), ec);

    env::Environment environment;
    environment.add<env::Scheduler>(env::SchedulerConfig{});
    Settings settings = Settings::load();
    InstrumentLibrary library(environment, settings);
    AudioPool pool(environment);
    const uint32_t rate = 48000;

    struct Harness {
        std::unique_ptr<OfflineEngine> engine; // with its pool
        std::unique_ptr<OfflineEngine> plain;  // the same graph, no pool
        uint64_t frames = 0;                   // the demo's song, and its tail
        uint64_t tail = 0;
        int phase = 0;
        std::chrono::steady_clock::time_point started;
    } h;

    auto seconds = [](std::chrono::steady_clock::time_point since) {
        return std::chrono::duration<double>(std::chrono::steady_clock::now() - since).count();
    };

    auto step = [&](env::Environment &) -> bool {
        if (h.phase == 0) {
            const Project demo = songToRender();
            h.started = std::chrono::steady_clock::now();
            h.engine = std::make_unique<OfflineEngine>(environment, nullptr, library, pool, demo, 0.8f, rate);
            h.plain = std::make_unique<OfflineEngine>(environment, nullptr, library, pool, demo, 0.8f, rate, false);
            h.phase = 1;
            return false;
        }
        if (h.phase == 1) {
            // Both graphs synced until every instrument and sound is in
            std::string waiting, plainWaiting;
            const bool engineReady = h.engine->prepare(waiting);
            const bool plainReady = h.plain->prepare(plainWaiting);
            if (engineReady && plainReady) {
                h.phase = 2;
            } else if (seconds(h.started) > 90.0) {
                c.check(false, "Instruments and sounds load in time", waiting + plainWaiting);
                return true;
            }
            return false;
        }
        if (h.phase == 2) {
            // The song's master, rendered with the pool and without it, over the song and its tail
            const Project &project = h.engine->store().project();
            ExportTarget song;
            song.range = ExportRangeKind::Song;
            double startBeat = 0.0, endBeat = 0.0;
            exportTargetRange(project, song, startBeat, endBeat);
            h.frames = static_cast<uint64_t>(std::ceil(project.timing.secondsAt(endBeat) * rate));
            h.tail = static_cast<uint64_t>(4.0 * rate);

            PassOutput master;
            master.id = 0;
            master.master = true;
            master.to = h.frames;
            master.tail = h.tail;
            master.path = dir + "/master.tmp";
            RenderPassConfig config;
            config.sampleRate = rate;
            config.outputs.push_back(master);
            std::string error;
            RenderPass plainPass(*h.plain, config);
            const bool plainOk = plainPass.run(error);
            std::vector<float> reference;
            const bool plainLoaded = plainOk && loadRender(master.path, reference);

            PassOutput pooled = master;
            pooled.path = dir + "/master-pooled.tmp";
            RenderPassConfig pooledConfig = config;
            pooledConfig.outputs = {pooled};
            RenderPass pooledPass(*h.engine, pooledConfig);
            const bool ok = pooledPass.run(error);
            std::vector<float> mixed;
            const bool loaded = ok && loadRender(pooled.path, mixed);
            double worst = 0.0;
            const size_t common = std::min(mixed.size(), reference.size());
            for (size_t k = 0; loaded && plainLoaded && k < common; ++k)
                worst = std::max(worst, static_cast<double>(std::fabs(mixed[k] - reference[k])));
            const long long lengthDiff = static_cast<long long>(mixed.size()) - static_cast<long long>(reference.size());
            char detail[160];
            std::snprintf(detail, sizeof detail, "(max diff %.2g, %zu and %zu frames%s)", worst, mixed.size() / 2,
                          reference.size() / 2, ok && plainOk ? "" : ", render failed");
            c.check(loaded && plainLoaded && worst <= 1e-6 && lengthDiff == 0,
                    "The pooled engine renders as the engine with no pool does", detail);
            h.phase = 3;
            return false;
        }
        if (h.phase == 3) {
            // Every track's post-fader output, unmuted, with the dry mix: the stems must add up to it
            const Project &project = h.engine->store().project();
            RenderPassConfig config;
            config.sampleRate = rate;
            config.unmuted = true;
            std::vector<int> all;
            for (size_t i = 0; i < project.tracks.size(); ++i) {
                PassOutput stem;
                stem.id = static_cast<int>(i) + 1;
                stem.master = false;
                stem.tracks = {static_cast<int>(i)};
                stem.respectMuteSolo = false;
                stem.to = h.frames;
                stem.tail = h.tail;
                stem.path = dir + "/stem" + std::to_string(i) + ".tmp";
                config.outputs.push_back(stem);
                all.push_back(static_cast<int>(i));
            }
            PassOutput dry;
            dry.id = 1000;
            dry.master = false;
            dry.tracks = all;
            dry.respectMuteSolo = false;
            dry.to = h.frames;
            dry.tail = h.tail;
            dry.path = dir + "/dry.tmp";
            config.outputs.push_back(dry);
            RenderPass pass(*h.engine, config);
            std::string error;
            const bool ok = pass.run(error);
            std::vector<float> dryMix, sum;
            bool loaded = ok && loadRender(dry.path, dryMix);
            // A stem ends once its track has been quiet for a fifth of a second, so the frames
            // every stem covers are compared exactly, and the rest only to the quiet level
            size_t shortest = std::numeric_limits<size_t>::max();
            for (const PassOutput &stem : config.outputs) {
                if (!loaded || stem.id == 1000)
                    continue;
                std::vector<float> samples;
                loaded = loadRender(stem.path, samples);
                shortest = std::min(shortest, samples.size());
                if (sum.size() < samples.size())
                    sum.resize(samples.size(), 0.0f);
                for (size_t k = 0; k < samples.size(); ++k)
                    sum[k] += samples[k];
            }
            double worst = 0.0, beyond = 0.0;
            for (size_t k = 0; loaded && k < std::max(sum.size(), dryMix.size()); ++k) {
                const float a = k < sum.size() ? sum[k] : 0.0f;
                const float b = k < dryMix.size() ? dryMix[k] : 0.0f;
                const double diff = std::fabs(static_cast<double>(a) - b);
                if (k < shortest)
                    worst = std::max(worst, diff);
                else
                    beyond = std::max(beyond, diff);
            }
            char detail[160];
            std::snprintf(detail, sizeof detail, "(max diff %.2g while every track runs, %.2g after the first ends)",
                          worst, beyond);
            c.check(loaded && worst <= 1e-5 && beyond <= 1e-4, "Every track's output adds up to the dry mix", detail);
            h.phase = 4;
            return false;
        }
        if (h.phase == 4) {
            // A long render, cancelled while it runs
            PassOutput long_;
            long_.id = 0;
            long_.master = true;
            long_.to = 600ull * rate;
            long_.path = dir + "/long.tmp";
            RenderPassConfig config;
            config.sampleRate = rate;
            config.outputs.push_back(long_);
            RenderPass pass(*h.engine, config);
            std::string error;
            bool finished = true;
            std::thread worker([&] { finished = pass.run(error); });
            std::this_thread::sleep_for(std::chrono::milliseconds(150));
            const auto cancelAt = std::chrono::steady_clock::now();
            pass.cancel();
            worker.join();
            const double ms = seconds(cancelAt) * 1000.0;
            const bool left = std::filesystem::exists(std::filesystem::u8path(long_.path)) ||
                              std::filesystem::exists(std::filesystem::u8path(long_.path + ".part"));
            char detail[120];
            std::snprintf(detail, sizeof detail, "(stopped in %.0f ms, %.1f s of audio rendered)", ms,
                          static_cast<double>(pass.renderedFrames()) / rate);
            c.check(!finished && ms < 500.0 && !left, "A cancelled render stops within a block and leaves no temp files",
                    detail);
            h.phase = 5;
            return false;
        }
        // The speed of a pass is a benchmark (VYBN_TEST_BENCH): nothing to assert, and a whole extra render
        if (!bench()) {
            std::filesystem::remove_all(std::filesystem::u8path(dir), ec);
            return true;
        }
        PassOutput master;
        master.id = 0;
        master.master = true;
        master.to = h.frames;
        master.tail = h.tail;
        master.path = dir + "/bench-master.tmp";
        RenderPassConfig config;
        config.sampleRate = rate;
        config.outputs.push_back(master);
        const size_t tracks = h.engine->trackCount();
        for (size_t i = 0; i < tracks; ++i) {
            PassOutput stem;
            stem.id = static_cast<int>(i) + 1;
            stem.master = false;
            stem.tracks = {static_cast<int>(i)};
            stem.to = h.frames;
            stem.tail = h.tail;
            stem.path = dir + "/bench-" + std::to_string(i) + ".tmp";
            config.outputs.push_back(stem);
        }
        RenderPass pass(*h.engine, config);
        std::string error;
        const auto begin = std::chrono::steady_clock::now();
        const bool ok = pass.run(error);
        const double wall = seconds(begin);
        const double audioSeconds = static_cast<double>(pass.renderedFrames()) / rate;
        c.check(ok, "The benchmark pass renders", error);
        std::printf("INFO  offline render speed: %.1f s of audio per second of wall time (demo master and %zu tracks, one pass)\n",
                    audioSeconds / std::max(wall, 1e-6), tracks);
        std::filesystem::remove_all(std::filesystem::u8path(dir), ec);
        return true;
    };
    environment.add<StepDaemon>(std::function<bool(env::Environment &)>(step));
    environment.run();
}

// The render queue end to end, through the environment's loop as the app runs it: the Release
// preset's loudness, true peak and FLAC against its WAV; Release and Stems together (timed); the
// MIDI preset; two sample rates in one preset; 16-bit WAV against FLAC; and a cancel mid-way that
// must leave nothing behind. Each step queues one job, drives the queue until it has finished, then
// checks it; a step returns true once it is done.
void queueChecks(Checker &c)
{
    using namespace daw;
    namespace fs = std::filesystem;
    using Clock = std::chrono::steady_clock;

    const std::string dir = (fs::temp_directory_path() / fs::u8path("vybn-queue-selftest")).u8string();
    std::error_code ec;
    fs::remove_all(fs::u8path(dir), ec);
    fs::create_directories(fs::u8path(dir), ec);

    env::Environment environment;
    environment.add<env::Scheduler>(env::SchedulerConfig{});
    Settings settings = Settings::load();
    InstrumentLibrary library(environment, settings);
    AudioPool pool(environment);
    ExportQueue queue(environment, nullptr, library, pool);
    const Project demo = songToRender();

    auto secondsSince = [](Clock::time_point since) {
        return std::chrono::duration<double>(Clock::now() - since).count();
    };

    // A preset of the named presets' targets, written into the test folder; `fileName`, when set,
    // replaces each target's name template
    auto presetFor = [&](const std::string &name, const std::vector<std::string> &from, const std::string &fileName,
                         const std::string &folder) {
        ExportPreset preset;
        preset.name = name;
        for (const ExportPreset &known : defaultExportPresets()) {
            if (std::find(from.begin(), from.end(), known.name) == from.end())
                continue;
            for (ExportTarget t : known.targets) {
                t.folder = folder;
                if (!fileName.empty())
                    t.fileName = fileName;
                preset.targets.push_back(t);
            }
        }
        return preset;
    };
    auto jobWithId = [&](uint64_t id) -> RenderJob * {
        for (RenderJob *job : queue.jobs()) {
            if (job->id() == id)
                return job;
        }
        return nullptr;
    };
    // The file names of a job's outputs that exist, for checks that nothing was left behind
    auto existing = [](const std::vector<TargetReport> &reports) {
        int count = 0;
        for (const TargetReport &r : reports) {
            if (!r.path.empty() && fs::exists(fs::u8path(r.path)))
                ++count;
        }
        return count;
    };

    // A job queued by its step: queued on the first call, then found by its id
    struct Queued {
        uint64_t id = 0;
        bool queued = false;
        Clock::time_point began;
    };
    // Queues the preset the first time, then returns the job once it has finished or the limit passed
    auto waitFor = [&](Queued &q, const ExportPreset &preset, double limitSeconds, RenderJob *&job) {
        if (!q.queued) {
            q.id = queue.enqueue(demo, preset, 0.8f, dir);
            q.began = Clock::now();
            q.queued = true;
        }
        job = jobWithId(q.id);
        return job == nullptr || job->finished() || secondsSince(q.began) > limitSeconds;
    };
    auto contentTracks = [&] {
        int count = 0;
        for (const Track &t : demo.tracks) {
            if (std::any_of(t.clips.begin(), t.clips.end(), [](const Clip &k) { return k.hasAudio() || !k.notes.empty(); }))
                ++count;
        }
        return count;
    };
    // Two numbers in one detail string, by a format with two %f
    auto two = [](const char *format, double a, double b) {
        char buffer[160];
        std::snprintf(buffer, sizeof buffer, format, a, b);
        return std::string(buffer);
    };
    // The song's length in seconds: its range as the Song target takes it (to the last clip's end)
    auto songSeconds = [&] {
        ExportTarget song;
        double startBeat = 0.0, endBeat = 0.0;
        exportTargetRange(demo, song, startBeat, endBeat);
        return demo.timing.secondsAt(endBeat);
    };

    std::vector<std::function<bool()>> steps;
    Queued release, combined, midi, rates, bits16, twice;

    // 1. The Release preset: a loudness-normalized WAV and a noise-shaped FLAC
    steps.push_back([&]() -> bool {
        RenderJob *job = nullptr;
        if (!waitFor(release, presetFor("Release", {"Release"}, "", dir), 400.0, job))
            return false;
        const bool done = job->state() == RenderJob::Done;
        const std::vector<TargetReport> reports = job->reports();
        c.check(done && reports.size() == 2 && reports[0].ok && reports[1].ok, "The Release preset renders its WAV and FLAC",
                job->status());
        if (!done || reports.size() != 2)
            return true;

        std::string wavError, flacError;
        auto wav = audio::loadAudioFile(reports[0].path, wavError);
        auto flac = audio::loadAudioFile(reports[1].path, flacError);
        c.check(wav && flac, "Both Release files decode", wavError + flacError);
        if (wav) {
            audio::master::LoudnessMeter meter(wav->channels, wav->sampleRate);
            meter.add(wav->samples.data(), wav->frames);
            audio::master::TruePeakMeter peak(wav->channels);
            peak.add(wav->samples.data(), wav->frames);
            c.check(near(meter.integrated(), -14.0, 0.3), "The Release WAV measures -14 LUFS (within 0.3 LU)",
                    number("(%.2f LUFS)", meter.integrated()));
            c.check(peak.peakDb() <= -0.9, "The Release WAV's true peak is at or under -0.9 dBTP",
                    number("(%.2f dBTP)", peak.peakDb()));
            c.check(reports[0].clipped == 0, "The limited WAV clips nothing", number("(%.0f samples)", static_cast<double>(reports[0].clipped)));
            std::printf("INFO  Release WAV: %.2f LUFS integrated, %.2f LU range, %.2f dBTP, gain %+.2f dB, %.1f s\n",
                        reports[0].integratedLufs, reports[0].loudnessRange, reports[0].truePeakDb, reports[0].gainDb,
                        reports[0].seconds);
        }
        if (wav && flac) {
            double worst = 0.0;
            const size_t common = std::min(wav->samples.size(), flac->samples.size());
            for (size_t k = 0; k < common; ++k)
                worst = std::max(worst, static_cast<double>(std::fabs(wav->samples[k] - flac->samples[k])));
            const double lsb16 = 1.0 / 32768.0;
            c.check(wav->frames == flac->frames && flac->channels == wav->channels && worst <= 3.0 * lsb16,
                    "The FLAC 16 decodes to the WAV's signal within 3 LSB of 16 bits",
                    number("(worst %.2f LSB)", worst / lsb16));
        }
        c.check(reports[0].bytes > 0 && reports[1].bytes > 0, "Each report has its file's size");
        // The song runs to its last clip's end, and its file adds the tail the notes ring on for
        const double song = songSeconds();
        c.check(reports[0].seconds >= song - 0.05 && reports[0].seconds <= song + 4.5,
                "The file runs the song and its tail", two("(%.2f s for a song of %.2f s)", reports[0].seconds, song));
        return true;
    });

    // 2. Release and Stems together: one file per track with notes or audio, and the masters (timed)
    steps.push_back([&]() -> bool {
        RenderJob *job = nullptr;
        if (!waitFor(combined, presetFor("Release+Stems", {"Release", "Stems"}, "", dir), 900.0, job))
            return false;
        const std::vector<TargetReport> reports = job->reports();
        // The two Release files, one per track with content, and the Stems master: all named apart by the defaults
        const size_t expected = static_cast<size_t>(contentTracks()) + 3;
        bool allOk = !reports.empty();
        std::set<std::string> names;
        for (const TargetReport &r : reports) {
            allOk = allOk && r.ok && fs::exists(fs::u8path(r.path));
            names.insert(r.path);
        }
        c.check(job->state() == RenderJob::Done && reports.size() == expected && allOk && names.size() == reports.size(),
                "Release and Stems write a file per track with content, and the masters, each under its own name",
                number("(%.0f files)", static_cast<double>(reports.size())));
        std::printf("INFO  whole-preset wall time for Release+Stems: %.1f s for a song of %.1f s, %zu files\n",
                    job->elapsedSeconds(), songSeconds(), reports.size());
        return true;
    });

    // 3. The MIDI preset: a Standard MIDI File of the instrument tracks
    steps.push_back([&]() -> bool {
        RenderJob *job = nullptr;
        if (!waitFor(midi, presetFor("MIDI", {"MIDI"}, "", dir), 120.0, job))
            return false;
        const std::vector<TargetReport> reports = job->reports();
        const bool ok = job->state() == RenderJob::Done && reports.size() == 1 && reports[0].ok && reports[0].midi;
        bool header = false;
        if (ok) {
            const std::vector<unsigned char> bytes = readFile(reports[0].path);
            header = bytes.size() > 14 && bytes[0] == 'M' && bytes[1] == 'T' && bytes[2] == 'h' && bytes[3] == 'd';
        }
        c.check(ok && header, "The MIDI preset writes a Standard MIDI File", job->status());
        return true;
    });

    // 4. Two sample rates in one preset: each file at its own rate, the same length in seconds
    steps.push_back([&]() -> bool {
        RenderJob *job = nullptr;
        ExportPreset preset;
        preset.name = "Two rates";
        ExportTarget base = defaultExportPresets().front().targets.front();
        base.folder = dir;
        base.range = ExportRangeKind::Bars;
        base.fromBar = 1;
        base.toBar = 5;
        base.fileName = "two rates {rate}";
        for (uint32_t rate : {44100u, 48000u}) {
            ExportTarget t = base;
            t.sampleRate = rate;
            preset.targets.push_back(t);
        }
        if (!waitFor(rates, preset, 300.0, job))
            return false;
        const std::vector<TargetReport> reports = job->reports();
        const bool ok = job->state() == RenderJob::Done && reports.size() == 2 && reports[0].ok && reports[1].ok;
        c.check(ok, "Two sample rates in one preset render both files", job->status());
        if (ok) {
            std::string e1, e2;
            auto a = audio::loadAudioFile(reports[0].path, e1);
            auto b = audio::loadAudioFile(reports[1].path, e2);
            c.check(a && b && near(a->sampleRate, 44100.0, 0.5) && near(b->sampleRate, 48000.0, 0.5),
                    "Each file is written at its own sample rate", e1 + e2);
            c.check(a && b && std::fabs(a->seconds() - b->seconds()) < 0.05, "Both files are the same length in seconds",
                    a && b ? two("(%.3f and %.3f s)", a->seconds(), b->seconds()) : std::string());
        }
        return true;
    });

    // 5. 16-bit WAV and 16-bit FLAC of the same target, dithered with the same seed: the same samples
    steps.push_back([&]() -> bool {
        RenderJob *job = nullptr;
        ExportPreset preset;
        preset.name = "Sixteen";
        ExportTarget base = defaultExportPresets().front().targets.front();
        base.folder = dir;
        base.range = ExportRangeKind::Bars;
        base.fromBar = 1;
        base.toBar = 5;
        base.bits = 16;
        base.fileName = "sixteen {format}";
        ExportTarget flac = base;
        flac.format = ExportFormat::Flac;
        // And an MP3 of it, set to a rate MP3 can't take (rendered at 48 kHz instead)
        ExportTarget mp3 = base;
        mp3.format = ExportFormat::Mp3;
        mp3.bitrate = 256;
        mp3.sampleRate = 96000;
        preset.targets = {base, flac, mp3};
        if (!waitFor(bits16, preset, 300.0, job))
            return false;
        const std::vector<TargetReport> reports = job->reports();
        const bool ok = job->state() == RenderJob::Done && reports.size() == 3 && reports[0].ok && reports[1].ok;
        c.check(ok, "A 16-bit WAV and FLAC of one target render", job->status());
        if (reports.size() == 3) {
            // The MP3 against the WAV: the same length give or take the encoder's priming, the same level
            std::string e1, e2;
            auto wav = audio::loadAudioFile(reports[0].path, e1);
            auto mp3Data = reports[2].ok ? audio::loadAudioFile(reports[2].path, e2) : nullptr;
            auto rms = [](const std::vector<float> &v) {
                double sum = 0.0;
                for (float x : v)
                    sum += static_cast<double>(x) * x;
                return std::sqrt(sum / std::max<size_t>(v.size(), 1));
            };
            const bool shaped = wav && mp3Data && std::llround(mp3Data->sampleRate) == 48000 && mp3Data->channels == wav->channels &&
                                mp3Data->frames >= wav->frames && mp3Data->frames <= wav->frames + 4000;
            const double levelDb = shaped ? 20.0 * std::log10(rms(mp3Data->samples) / std::max(rms(wav->samples), 1e-12)) : 99.0;
            c.check(reports[2].ok && shaped && std::fabs(levelDb) < 0.5 && reports[2].path.size() > 4 &&
                        reports[2].path.substr(reports[2].path.size() - 4) == ".mp3",
                    "An MP3 target renders at 48 kHz, as long and as loud as the WAV",
                    reports[2].ok ? number("(level %+.2f dB)", levelDb) : reports[2].error);
        }
        if (ok) {
            std::string e1, e2;
            auto wav = audio::loadAudioFile(reports[0].path, e1);
            auto flac16 = audio::loadAudioFile(reports[1].path, e2);
            double worst = 0.0;
            if (wav && flac16 && wav->frames == flac16->frames) {
                for (size_t k = 0; k < wav->samples.size(); ++k)
                    worst = std::max(worst, static_cast<double>(std::fabs(wav->samples[k] - flac16->samples[k])));
            }
            const double lsb16 = 1.0 / 32768.0;
            c.check(wav && flac16 && wav->frames == flac16->frames && worst <= lsb16,
                    "The 16-bit WAV and FLAC decode to the same samples", number("(worst %.3f LSB)", worst / lsb16));
        }
        return true;
    });

    // 6. A cancel while the Release and Stems render: it stops within a block, and the temp folder,
    // the part files and the outputs go
    struct Cancel {
        int phase = 0;
        uint64_t id = 0;
        Clock::time_point began;
        Clock::time_point cancelledAt;
    };
    auto cancel = std::make_shared<Cancel>();
    steps.push_back([&, cancel]() -> bool {
        if (cancel->phase == 0) {
            cancel->id = queue.enqueue(demo, presetFor("Cancelled", {"Release", "Stems"}, "", dir + "/cancel"), 0.8f, dir);
            cancel->began = Clock::now();
            cancel->phase = 1;
            return false;
        }
        RenderJob *job = jobWithId(cancel->id);
        if (!job)
            return true;
        if (cancel->phase == 1) {
            // Cancelled once its render is well under way (it gives up after five minutes)
            const bool rendering = job->state() == RenderJob::Rendering && job->fraction() > 0.05f;
            if (!rendering && !job->finished() && secondsSince(cancel->began) < 300.0)
                return false;
            if (job->finished()) {
                c.check(false, "The job is still rendering when it is cancelled", job->status());
                return true;
            }
            job->cancel();
            cancel->cancelledAt = Clock::now();
            cancel->phase = 2;
            return false;
        }
        if (!job->finished())
            return false;
        const double ms = secondsSince(cancel->cancelledAt) * 1000.0;
        bool partFiles = false;
        for (const auto &entry : fs::directory_iterator(fs::u8path(dir + "/cancel"), ec)) {
            if (entry.path().extension() == ".part")
                partFiles = true;
        }
        const bool tempGone = !fs::exists(fs::u8path(job->tempFolder()));
        c.check(job->state() == RenderJob::Cancelled, "A cancelled render ends Cancelled", job->status());
        c.check(tempGone && !partFiles && existing(job->reports()) == 0,
                "A cancelled render leaves no temp folder, part files or outputs");
        c.check(ms < 3000.0, "A cancelled render stops quickly", number("(%.0f ms)", ms));
        c.check(queue.remove(cancel->id), "A finished job can be removed from the queue");
        return true;
    });

    // 7. A loud mix normalized to -9 LUFS with the limiter at -1 dBTP: dense noise has peaks the limiter
    // must take off, so the loudness comes out under the first estimate and the gain is corrected
    struct Loud {
        int phase = 0;
        uint64_t id = 0;
        Clock::time_point began;
    };
    auto loud = std::make_shared<Loud>();
    steps.push_back([&, loud]() -> bool {
        if (loud->phase == 0) {
            const size_t frames = 48000 * 8;
            std::vector<float> noise(frames * 2);
            uint32_t seed = 12345;
            auto uniform = [&seed] {
                seed = seed * 1664525u + 1013904223u;
                return static_cast<double>(seed >> 8) / 16777216.0 * 2.0 - 1.0;
            };
            // Heavy-tailed noise (a sum of four uniform values, squared with its sign) scaled to 0.1 rms:
            // its peaks sit about 10 dB over its level, so at -9 LUFS they are over the -1 dBTP ceiling
            double energy = 0.0;
            for (float &v : noise) {
                const double s = uniform() + uniform() + uniform() + uniform();
                v = static_cast<float>(s * std::fabs(s));
                energy += static_cast<double>(v) * v;
            }
            const auto scale = static_cast<float>(0.1 / std::sqrt(energy / static_cast<double>(noise.size())));
            for (float &v : noise)
                v *= scale;
            const std::string path = dir + "/loud-noise.wav";
            std::string error;
            if (!audio::writeWavFile(path, noise.data(), frames, 2, 48000.0, error, 32)) {
                c.check(false, "A loud test signal is written", error);
                return true;
            }
            Project mix;
            mix.tracks.clear();
            const int track = mix.addTrack(TrackKind::Audio);
            Clip clip;
            clip.name = "Noise";
            clip.start = 0.0;
            clip.length = 16.0;
            clip.audioFile = path;
            mix.tracks[static_cast<size_t>(track)].clips.push_back(clip);
            ExportTarget target = defaultExportPresets()[1].targets[0]; // Release's Master WAV
            target.folder = dir + "/loud";
            target.fileName = "loud";
            target.loudnessLufs = -9.0;
            target.ceilingDbtp = -1.0;
            ExportPreset preset;
            preset.name = "Loud";
            preset.targets = {target};
            loud->id = queue.enqueue(mix, preset, 0.8f, dir);
            loud->began = Clock::now();
            loud->phase = 1;
            return false;
        }
        RenderJob *job = jobWithId(loud->id);
        if (!job || (!job->finished() && secondsSince(loud->began) < 300.0))
            return false;
        const std::vector<TargetReport> reports = job->reports();
        const bool done = job->state() == RenderJob::Done && reports.size() == 1 && reports[0].ok;
        c.check(done, "A loud mix renders normalized to -9 LUFS", job->status());
        if (done) {
            std::string error;
            auto decoded = audio::loadAudioFile(reports[0].path, error);
            if (decoded) {
                audio::master::LoudnessMeter meter(decoded->channels, decoded->sampleRate);
                meter.add(decoded->samples.data(), decoded->frames);
                audio::master::TruePeakMeter peak(decoded->channels);
                peak.add(decoded->samples.data(), decoded->frames);
                c.check(near(meter.integrated(), -9.0, 0.3), "The loud mix lands within 0.3 LU of -9 LUFS",
                        number("(%.2f LUFS)", meter.integrated()));
                c.check(peak.peakDb() <= -0.9, "The loud mix's true peak is at or under -0.9 dBTP",
                        number("(%.2f dBTP)", peak.peakDb()));
                std::printf("INFO  loud mix: %.2f LUFS measured, gain %+.2f dB, %.2f dBTP\n", meter.integrated(),
                            reports[0].gainDb, peak.peakDb());
            } else {
                c.check(false, "The loud mix decodes", error);
            }
        }
        return true;
    });

    // 8. Two outputs of one job named alike: the second is numbered, and its report says so
    steps.push_back([&]() -> bool {
        RenderJob *job = nullptr;
        ExportPreset preset;
        preset.name = "Twice";
        ExportTarget base = defaultExportPresets().front().targets.front();
        base.folder = dir + "/twice";
        base.range = ExportRangeKind::Bars;
        base.fromBar = 1;
        base.toBar = 3;
        base.fileName = "same name";
        preset.targets = {base, base};
        if (!waitFor(twice, preset, 300.0, job))
            return false;
        const std::vector<TargetReport> reports = job->reports();
        const bool ok = job->state() == RenderJob::Done && reports.size() == 2 && reports[0].ok && reports[1].ok;
        c.check(ok, "Two targets with one name render two files", job->status());
        if (ok) {
            const bool numbered = reports[0].path != reports[1].path && reports[1].path.find(" (2).wav") != std::string::npos;
            c.check(numbered && reports[0].note.empty() && !reports[1].note.empty(),
                    "A repeated name is numbered, and the report says so", reports[1].path + " / " + reports[1].note);
        }
        return true;
    });

    // 9. A job destroyed while it renders: the engine's render stops, and the temp folder, the part
    // files and the outputs are all gone once it is destroyed
    struct Destroy {
        int phase = 0;
        std::unique_ptr<RenderJob> job;
        std::string temp;
        std::string folder;
        Clock::time_point began;
    };
    auto destroy = std::make_shared<Destroy>();
    steps.push_back([&, destroy]() -> bool {
        if (destroy->phase == 0) {
            destroy->folder = dir + "/destroyed";
            destroy->job = std::make_unique<RenderJob>(environment, nullptr, library, pool, demo,
                                                       presetFor("Destroyed", {"Release", "Stems"}, "destroyed {target}",
                                                                 destroy->folder),
                                                       0.8f, destroy->folder, 9001);
            destroy->began = Clock::now();
            destroy->phase = 1;
            return false;
        }
        // Driven here until it is well under way (it gives up after five minutes)
        if (destroy->phase == 1) {
            destroy->job->update();
            const bool rendering = destroy->job->state() == RenderJob::Rendering && destroy->job->fraction() > 0.05f;
            if (!rendering && secondsSince(destroy->began) < 300.0)
                return false;
            if (!rendering) {
                c.check(false, "The job is rendering when it is destroyed", destroy->job->status());
                destroy->job.reset();
                return true;
            }
            destroy->temp = destroy->job->tempFolder();
            destroy->job.reset(); // waits for the render to stop, then removes what it made
            destroy->phase = 2;
            return false;
        }
        bool partFiles = false;
        int entries = 0;
        for (const auto &entry : fs::directory_iterator(fs::u8path(destroy->folder), ec)) {
            ++entries;
            if (entry.path().extension() == ".part")
                partFiles = true;
        }
        c.check(!fs::exists(fs::u8path(destroy->temp)) && !partFiles && entries == 0,
                "A job destroyed while rendering leaves no temp folder, part files or outputs",
                number("(%.0f files left)", static_cast<double>(entries)));
        return true;
    });

    // 10. Freezing (docs/design/freeze-plan.md): an audio track of noise through a saturation is frozen,
    // bounced and flattened; the frozen song exports the same as the live one
    struct Frozen {
        int phase = 0;
        std::unique_ptr<Freezer> freezer;
        Project live;
        Freezer::Result freeze, bounce;
        uint64_t liveExport = 0, frozenExport = 0;
        Clock::time_point began;
    };
    auto frozen = std::make_shared<Frozen>();
    auto masterTarget = [&](const std::string &name) {
        ExportTarget target;
        target.name = name;
        target.format = ExportFormat::Wav;
        target.bits = 32;
        target.normalize = NormalizeMode::Off;
        target.limiter = false;
        target.dither = DitherMode::None;
        target.range = ExportRangeKind::Bars;
        target.fromBar = 1;
        target.toBar = 3;
        target.tailSeconds = 0.0;
        target.folder = dir + "/frozen";
        target.fileName = name;
        ExportPreset preset;
        preset.name = name;
        preset.targets = {target};
        return preset;
    };
    steps.push_back([&, frozen]() -> bool {
        Frozen &f = *frozen;
        if (f.phase == 0) {
            const size_t frames = 48000 * 3;
            std::vector<float> noise(frames * 2);
            uint32_t seed = 99;
            for (float &v : noise) {
                seed = seed * 1664525u + 1013904223u;
                v = static_cast<float>(static_cast<double>(seed >> 8) / 16777216.0 - 0.5) * 0.6f;
            }
            const std::string path = dir + "/freeze-noise.wav";
            std::string error;
            if (!audio::writeWavFile(path, noise.data(), frames, 2, 48000.0, error, 32)) {
                c.check(false, "A noise file for freezing is written", error);
                return true;
            }
            f.live.tracks.clear();
            f.live.timing = audio::TempoMap(120.0, 4, 4);
            const int track = f.live.addTrack(TrackKind::Audio);
            Track &t = f.live.tracks[static_cast<size_t>(track)];
            t.name = "Noise";
            t.gain = 0.7f;
            Clip clip("Noise", 0.0, 6.0); // 3 s at 120
            clip.audioFile = path;
            t.clips.push_back(clip);
            t.effects.push_back(audio::effectSetting(audio::EffectType::Saturation));
            ObjectId next = 1;
            f.live.normalizeIds(next);
            f.freezer = std::make_unique<Freezer>(environment, nullptr, library, pool);
            std::string why;
            const bool started = f.freezer->start(f.live, 0, FreezeKind::Freeze, dir + "/frozen", 48000, why) &&
                                 f.freezer->start(f.live, 0, FreezeKind::Bounce, dir + "/frozen", 48000, why) == false;
            c.check(started && f.freezer->busy(f.live.tracks[0].id), "Freezing: a render is queued, one per track at a time", why);
            f.began = Clock::now();
            f.phase = 1;
            return false;
        }
        if (f.phase == 1) {
            f.freezer->update();
            std::vector<Freezer::Result> done = f.freezer->takeFinished();
            if (done.empty()) {
                if (secondsSince(f.began) < 120.0)
                    return false;
                c.check(false, "Freezing: the render finishes", f.freezer->status(f.live.tracks[0].id));
                return true;
            }
            f.freeze = done.front();
            const bool ok = f.freeze.error.empty() && !f.freeze.file.empty() && fs::exists(fs::u8path(f.freeze.file)) &&
                            f.freeze.hash == freezeHash(f.live, 0) && f.freeze.seconds >= 3.0 && f.freeze.seconds < 3.5;
            c.check(ok, "Freezing: the track's sound is written, as long as it rings", f.freeze.error + number(" (%.2f s)", f.freeze.seconds));
            if (!ok)
                return true;
            // The same song, frozen: exported beside the live one
            Project frozenSong = f.live;
            frozenSong.tracks[0].freeze = TrackFreeze{f.freeze.file, f.freeze.seconds, f.freeze.hash};
            c.check(freezeCurrent(frozenSong, 0), "Freezing: the frozen track's freeze is current");
            f.liveExport = queue.enqueue(f.live, masterTarget("live"), 1.0f, dir);
            f.frozenExport = queue.enqueue(frozenSong, masterTarget("frozen"), 1.0f, dir);
            std::string why;
            f.freezer->start(f.live, 0, FreezeKind::Bounce, dir + "/frozen", 48000, why);
            f.began = Clock::now();
            f.phase = 2;
            return false;
        }
        if (f.phase == 2) {
            f.freezer->update();
            for (const Freezer::Result &r : f.freezer->takeFinished())
                f.bounce = r;
            RenderJob *a = jobWithId(f.liveExport);
            RenderJob *b = jobWithId(f.frozenExport);
            const bool finished = a && b && a->finished() && b->finished() && (!f.bounce.file.empty() || !f.bounce.error.empty());
            if (!finished && secondsSince(f.began) < 180.0)
                return false;
            const bool exported = a && b && a->state() == RenderJob::Done && b->state() == RenderJob::Done;
            c.check(exported, "Freezing: the live and frozen songs export", a ? a->status() : std::string("no job"));
            if (exported) {
                std::string error;
                auto liveSound = audio::loadAudioFile(a->reports()[0].path, error);
                auto frozenSound = audio::loadAudioFile(b->reports()[0].path, error);
                double worst = 1.0, level = 0.0;
                if (liveSound && frozenSound && liveSound->samples.size() == frozenSound->samples.size()) {
                    worst = 0.0;
                    for (size_t i = 0; i < liveSound->samples.size(); ++i) {
                        worst = std::max(worst, static_cast<double>(std::fabs(liveSound->samples[i] - frozenSound->samples[i])));
                        level = std::max(level, static_cast<double>(std::fabs(liveSound->samples[i])));
                    }
                }
                c.check(level > 0.05 && worst < 1e-4, "Freezing: the frozen song sounds as the live one did",
                        number("(differs by %.1f dB at most)", 20.0 * std::log10(std::max(worst, 1e-12))));
            }
            // Bounce: a new audio track of the sound after the source, which is muted
            Project bounced = f.live;
            const int added = f.bounce.file.empty() ? -1 : addBounce(bounced, 0, f.bounce.file, f.bounce.seconds);
            c.check(added == 1 && bounced.tracks.size() == 2 && bounced.tracks[0].mute && bounced.tracks[1].kind == TrackKind::Audio &&
                        bounced.tracks[1].clips.size() == 1 && bounced.tracks[1].clips[0].audioFile == f.bounce.file &&
                        bounced.tracks[1].name == "Noise (bounced)",
                    "Bouncing: a new audio track plays the sound, the source muted", f.bounce.error);
            // Flatten: the frozen track becomes an audio track of its freeze, its effects gone, its fader kept
            Project flat = f.live;
            flat.tracks[0].freeze = TrackFreeze{f.freeze.file, f.freeze.seconds, f.freeze.hash};
            const bool flattened = flattenTrack(flat, 0);
            c.check(flattened && flat.tracks[0].effects.empty() && !flat.tracks[0].frozen() && flat.tracks[0].gain == 0.7f &&
                        flat.tracks[0].clips.size() == 1 && flat.tracks[0].clips[0].audioFile == f.freeze.file,
                    "Flattening: the track plays its freeze as audio");
            // Editing the track puts its freeze out of date
            Project edited = f.live;
            edited.tracks[0].freeze = TrackFreeze{f.freeze.file, f.freeze.seconds, f.freeze.hash};
            edited.tracks[0].effects[0].setValue(0, edited.tracks[0].effects[0].value(0, 0.0) + 1.0);
            Project faded = f.live;
            faded.tracks[0].freeze = TrackFreeze{f.freeze.file, f.freeze.seconds, f.freeze.hash};
            faded.tracks[0].gain = 0.2f;
            c.check(!freezeCurrent(edited, 0) && freezeCurrent(faded, 0),
                    "Freezing: an effect's change puts the freeze out of date, the fader doesn't");
            f.freezer.reset();
            return true;
        }
        return true;
    });

    size_t at = 0;
    environment.add<StepDaemon>(std::function<bool(env::Environment &)>([&](env::Environment &) -> bool {
        queue.update();
        if (at < steps.size() && steps[at]())
            ++at;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        return at >= steps.size();
    }));
    environment.run();
}

} // namespace

int runExportSelfTest()
{
    Checker c;

    // K-weighting: the published 48 kHz coefficients of BS.1770
    audio::master::KWeightCoefficients shelf{}, highPass{};
    audio::master::kWeightingCoefficients(kRate, shelf, highPass);
    c.check(near(shelf.b0, 1.53512485958697, 1e-4) && near(shelf.b1, -2.69169618940638, 1e-4) &&
                near(shelf.b2, 1.19839281085285, 1e-4) && near(shelf.a1, -1.69065929318241, 1e-4) &&
                near(shelf.a2, 0.73248077421585, 1e-4),
            "The K-weighting pre-filter matches BS.1770 at 48 kHz",
            number("(b0 %.6f)", shelf.b0));
    c.check(near(highPass.b0, 1.0, 1e-4) && near(highPass.b1, -2.0, 1e-4) && near(highPass.b2, 1.0, 1e-4) &&
                near(highPass.a1, -1.99004745483398, 1e-4) && near(highPass.a2, 0.99007225036621, 1e-4),
            "The RLB high-pass matches BS.1770 at 48 kHz", number("(a1 %.6f)", highPass.a1));

    // Integrated loudness of a 997 Hz stereo sine: its peak per channel in dBFS is its loudness
    const uint32_t stereo = 2;
    const size_t tenSeconds = static_cast<size_t>(kRate) * 10;
    for (double level : {-23.0, -33.0}) {
        audio::master::LoudnessMeter meter(stereo, kRate);
        const std::vector<float> tone = sine(tenSeconds, stereo, 997.0, std::pow(10.0, level / 20.0));
        meter.add(tone.data(), tenSeconds);
        const double lufs = meter.integrated();
        c.check(near(lufs, level, 0.1),
                number("A 997 Hz stereo sine at %.0f dBFS measures its level in LUFS", level), number("(%.2f LUFS)", lufs));
    }

    {
        audio::master::LoudnessMeter meter(stereo, kRate);
        const std::vector<float> silence(tenSeconds * stereo, 0.0f);
        meter.add(silence.data(), tenSeconds);
        const double lufs = meter.integrated();
        c.check(!(lufs > -70.0), "Silence is below -70 LUFS (negative infinity)");
    }

    // True peak: a sine at -6 dBFS reaches -6 dBTP; a quarter-rate sine at 45 degrees has its
    // samples at 0.707 but reaches 1.0 between them
    {
        const std::vector<float> tone = sine(static_cast<size_t>(kRate), stereo, 997.0, std::pow(10.0, -6.0 / 20.0));
        const double tp = truePeakDb(tone, stereo);
        c.check(near(tp, -6.0, 0.1), "A sine at -6 dBFS has a true peak of -6 dBTP", number("(%.2f dBTP)", tp));
    }
    {
        const std::vector<float> between = sine(static_cast<size_t>(kRate), stereo, kRate / 4.0, 1.0, kPi / 4.0);
        const double sp = audio::toDecibels(static_cast<float>(audio::master::samplePeak(between.data(), between.size() / stereo, stereo)));
        const double tp = truePeakDb(between, stereo);
        c.check(tp > sp && near(tp - sp, 3.0, 0.3),
                "An inter-sample peak shows about 3 dB above the sample peak",
                number("(true %.2f dBTP)", tp));
    }

    // The limiter: a loud inter-sample signal held under a ceiling of -1 dBTP, as long as its input
    {
        const std::vector<float> loud = sine(static_cast<size_t>(kRate), stereo, kRate / 4.0, 1.0, kPi / 4.0);
        const std::vector<float> out = limit(loud, stereo, -1.0);
        const double tp = truePeakDb(out, stereo);
        c.check(out.size() == loud.size(), "The limited output has exactly as many frames as the input");
        c.check(tp <= -1.0 + 0.1, "The limiter keeps the true peak under its ceiling", number("(%.2f dBTP)", tp));
        c.check(tp >= -1.0 - 0.5, "The limiter reaches its ceiling rather than going far under", number("(%.2f dBTP)", tp));
    }

    // Quiet material passes untouched, sample for sample (the output is aligned to the input)
    {
        const std::vector<float> quiet = sine(static_cast<size_t>(kRate), stereo, 997.0, 0.1);
        const std::vector<float> out = limit(quiet, stereo, -1.0);
        bool identical = out.size() == quiet.size();
        for (size_t i = 0; identical && i < quiet.size(); ++i)
            identical = out[i] == quiet[i];
        c.check(identical, "Quiet material comes through bit-identical, aligned to its input");
    }

    // Dither on 16 bits: the error stays within 1.5 LSB, and it varies from sample to sample
    {
        const double value = 0.1234567;
        const double lsb = 1.0 / 32768.0;
        std::vector<float> samples(20000, static_cast<float>(value));
        audio::master::Dither dither(audio::master::DitherMode::Tpdf, 1, 7);
        dither.apply(samples.data(), samples.size(), 1, 16);
        double worst = 0.0;
        std::set<float> distinct;
        for (float s : samples) {
            worst = std::max(worst, std::fabs(static_cast<double>(s) - value) / lsb);
            distinct.insert(s);
        }
        c.check(worst <= 1.5, "TPDF dither on 16 bits keeps its error within 1.5 LSB", number("(worst %.2f LSB)", worst));
        c.check(distinct.size() > 1, "TPDF dither's error isn't constant", number("(%.0f values)", static_cast<double>(distinct.size())));
    }

    // Fades: equal power, starting at silence and ending at silence, and the same across blocks
    {
        const size_t total = 48000;
        std::vector<float> whole(total * stereo, 1.0f);
        audio::master::applyFade(whole.data(), total, stereo, 4800, 4800, total, 0);
        c.check(whole[0] == 0.0f && whole[1] == 0.0f, "A fade in starts at silence");
        c.check(std::fabs(whole[(total - 1) * stereo]) < 1e-3f, "A fade out ends at silence",
                number("(last %.5f)", whole[(total - 1) * stereo]));
        std::vector<float> blocks(total * stereo, 1.0f);
        const size_t half = total / 2;
        audio::master::applyFade(blocks.data(), half, stereo, 4800, 4800, total, 0);
        audio::master::applyFade(blocks.data() + half * stereo, total - half, stereo, 4800, 4800, total, half);
        bool same = true;
        for (size_t i = 0; same && i < whole.size(); ++i)
            same = std::fabs(whole[i] - blocks[i]) < 1e-6f;
        c.check(same, "Fades in blocks match the fade of the whole file");
    }

    renderChecks(c);
    encoderChecks(c);
    midiChecks(c);
    queueChecks(c);
    return c.finish("export");
}

}
