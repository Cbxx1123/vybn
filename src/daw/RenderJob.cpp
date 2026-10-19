#include "RenderJob.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <limits>
#include <cctype>
#include <memory>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "Library.h"
#include "MidiExport.h"
#include "audio/AudioProcessor.h"
#include "audio/Encoders.h"
#include "audio/Mastering.h"
#include "audio/Memory.h"
#include "plugin/Slot.h"

namespace daw {

namespace {

// Silence for the tail: -80 dB, and the tail ends once it has stayed there for a fifth of a second
constexpr float kQuietLevel = 1e-4f;
constexpr uint32_t kBlock = audio::AudioSystem::kMaxBlock;

// Set on each of the engine's render threads, so their streamed samples are fetched when asked for
void markWorkerOffline()
{
    audio::setRenderingOffline(true);
}

uint32_t workerCount()
{
    // Half the cores less one for the live engine's own threads, at least one
    const int cores = static_cast<int>(std::thread::hardware_concurrency()) / 2 - 1;
    return static_cast<uint32_t>(std::max(1, cores));
}

// What one output of a pass writes and measures
struct Output {
    PassOutput spec;
    std::vector<int> summed;                  // the tracks that count (when not master)
    std::vector<float> mix;                   // this block's frames of the output, interleaved
    std::unique_ptr<audio::FloatTempWriter> writer;
    std::unique_ptr<audio::master::LoudnessMeter> loudness;
    std::unique_ptr<audio::master::TruePeakMeter> truePeak;
    uint64_t frames = 0;
    float samplePeak = 0.0f;
    uint64_t clipped = 0;
    uint64_t quiet = 0; // tail frames in a row below the quiet level
    bool done = false;
};

// Removes the temp files of a pass that didn't finish. The writers still open remove their own
// partial files when they go.
void discard(std::vector<std::unique_ptr<Output>> &outputs)
{
    for (auto &o : outputs)
        o->writer.reset();
    for (auto &o : outputs) {
        std::error_code ec;
        std::filesystem::remove(std::filesystem::u8path(o->spec.path), ec);
        std::filesystem::remove(std::filesystem::u8path(o->spec.path + ".part"), ec);
    }
}

}

bool renderReady(const Project &project, AudioBridge &bridge, InstrumentLibrary &library, AudioPool &pool,
                 std::string &waiting)
{
    const auto &installed = library.instruments();
    for (size_t i = 0; i < project.tracks.size(); ++i) {
        const Track &t = project.tracks[i];
        const std::string &name = t.instrument.id;
        if (t.kind == TrackKind::Instrument && t.instrument.kind == audio::DeviceKind::Sampler && !t.clips.empty()) {
            // Not installed: the track is silent, as it plays live
            if (std::find(installed.begin(), installed.end(), name) == installed.end())
                continue;
            if (bridge.instrumentFailed(i))
                continue;
            if (!bridge.instrumentReady(i, name)) {
                waiting = name;
                return false;
            }
        }
        for (const Clip &c : t.clips) {
            if (c.hasAudio() && !pool.get(c.audioFile) && pool.error(c.audioFile).empty()) {
                waiting = std::filesystem::path(c.audioFile).filename().string();
                return false;
            }
        }
    }
    return true;
}

OfflineEngine::OfflineEngine(env::Environment &env, plugin::PluginManager *plugins, InstrumentLibrary &library,
                             AudioPool &pool, Project snapshot, float masterGain, uint32_t sampleRate, bool pooled)
    : store_(std::move(snapshot)), workers_(false), audio_(env, audio::Deviceless{}), library_(library), sounds_(pool),
      plugins_(plugins), rate_(sampleRate)
{
    if (pooled) {
        workers_.onWorkerStart(&markWorkerOffline);
        workers_.start(workerCount());
        audio_.setPool(&workers_);
    }

    // A render never plays what is armed for recording
    for (size_t i = 0; i < store_.project().tracks.size(); ++i) {
        if (store_.project().tracks[i].armed)
            store_.editMix(static_cast<int>(i)).armed = false;
    }

    // The hub goes first, before any track, as DawApp adds it (the tracks read its messages)
    midi_ = &audio_.add<audio::MidiHub>();
    bridge_ = std::make_unique<AudioBridge>(audio_, *midi_, plugins_, library_, sounds_);
    audio_.masterGain().set(masterGain);
    audio_.transport().setTempo(store_.project().tempo);
    audio_.masterEffects().setOffline(true);
    audio_.prepareOffline(sampleRate);
}

OfflineEngine::~OfflineEngine() = default;

bool OfflineEngine::prepare(std::string &waiting)
{
    if (ready_)
        return true;
    // The pool's version before the sync: a sound file that loads during this frame is picked up by the next
    const uint64_t soundVersion = sounds_.version();
    AudioBridge::SyncOptions options;
    options.listen = false;
    bridge_->sync(store_, options);
    audio_.collectOffline();

    // Plugins are waited for, not passed through: every chain and slot renders offline
    for (size_t i = 0; i < trackCount(); ++i) {
        if (audio::DeviceChain *chain = bridge_->chainOf(false, static_cast<int>(i)))
            chain->setOffline(true);
    }
    bool pluginBusy = false;
    std::string busyName;
    for (plugin::Slot *slot : bridge_->pluginSlots()) {
        slot->setOffline(true);
        if (!pluginBusy && slot->busy()) {
            pluginBusy = true;
            busyName = slot->name();
        }
    }

    const bool soundsReady = renderReady(store_.project(), *bridge_, library_, sounds_, waiting);
    if (pluginBusy) {
        waiting = busyName;
        return false;
    }
    if (!soundsReady)
        return false;
    if (sounds_.version() != soundVersion) {
        waiting = "the sound files";
        return false;
    }
    ready_ = true;
    return true;
}

void OfflineEngine::beginRender(double startBeat, const std::vector<int> &tracks, bool preFader, bool unmuted)
{
    const Project &project = store_.project();
    taps_.resize(trackCount());
    for (size_t i = 0; i < trackCount(); ++i) {
        audio::TrackProcessor *processor = bridge_->processor(static_cast<int>(i));
        if (!processor)
            continue;
        const bool tapped = std::find(tracks.begin(), tracks.end(), static_cast<int>(i)) != tracks.end();
        processor->mute = unmuted ? false : project.tracks[i].mute;
        TapBuffers &buffers = taps_[i];
        if (tapped) {
            buffers.left.resize(kBlock);
            buffers.right.resize(kBlock);
            processor->setTap(buffers.left.data(), buffers.right.data(), preFader);
        } else {
            buffers.left.clear();
            buffers.right.clear();
            processor->setTap(nullptr, nullptr, false);
        }
    }

    // A quarter second stopped first, as the old exporter did, so notes from before let go and
    // levels settle. The transport stays stopped, so the position stays at 0.
    audio::Transport &transport = audio_.transport();
    transport.stop();
    transport.setLooping(false);
    transport.locate(0);
    std::vector<float> settle(kBlock * 2);
    for (uint32_t done = 0; done < rate_ / 4; done += 1024)
        audio_.renderOffline(settle.data(), 1024, rate_);

    const double seconds = startBeat * project.secondsPerBeat();
    transport.locate(static_cast<uint64_t>(std::llround(seconds * rate_)));
    transport.play();
}

void OfflineEngine::endRender()
{
    audio_.transport().stop();
    for (size_t i = 0; i < trackCount(); ++i) {
        if (audio::TrackProcessor *processor = bridge_->processor(static_cast<int>(i)))
            processor->setTap(nullptr, nullptr, false);
    }
}

void OfflineEngine::render(float *out, uint32_t frames)
{
    audio_.renderOffline(out, frames, rate_);
}

const float *OfflineEngine::tapLeft(int track) const
{
    if (track < 0 || static_cast<size_t>(track) >= taps_.size() || taps_[static_cast<size_t>(track)].left.empty())
        return nullptr;
    return taps_[static_cast<size_t>(track)].left.data();
}

const float *OfflineEngine::tapRight(int track) const
{
    if (track < 0 || static_cast<size_t>(track) >= taps_.size() || taps_[static_cast<size_t>(track)].right.empty())
        return nullptr;
    return taps_[static_cast<size_t>(track)].right.data();
}

void OfflineEngine::setRate(uint32_t rate)
{
    if (rate == rate_)
        return;
    // The graph is prepared at the new rate now; prepare() on the main thread waits for the plugins
    // that are activating again to settle, since only the main thread may read their state
    audio_.prepareOffline(rate);
    rate_ = rate;
    ready_ = false;
}

RenderPass::RenderPass(OfflineEngine &engine, RenderPassConfig config) : engine_(engine), config_(std::move(config))
{
}

bool RenderPass::run(std::string &error)
{
    // This thread renders offline: its streamed samples are fetched as the render asks for them
    const bool wasOffline = audio::renderingOffline();
    audio::setRenderingOffline(true);
    struct Restore {
        bool on;
        ~Restore() { audio::setRenderingOffline(on); }
    } restore{wasOffline};

    results_.clear();
    rendered_ = 0;
    fraction_.store(0.0f, std::memory_order_relaxed);
    cancelled_.store(false, std::memory_order_relaxed);

    const Project &project = engine_.store().project();
    const uint32_t rate = config_.sampleRate;
    bool anySolo = false;
    for (const Track &t : project.tracks)
        anySolo = anySolo || t.solo;

    // Mute is the engine's (a track's tap is silent while muted), so outputs that ignore it need an
    // unmuted pass, and the others a pass that respects it
    std::vector<std::unique_ptr<Output>> outputs;
    std::vector<int> tapped;
    uint64_t length = 1;
    for (const PassOutput &spec : config_.outputs) {
        const bool needsUnmuted = !spec.master && !spec.respectMuteSolo;
        if (needsUnmuted != config_.unmuted) {
            error = "Outputs with different mute rules need passes of their own";
            return false;
        }
        auto o = std::make_unique<Output>();
        o->spec = spec;
        if (!spec.master) {
            for (int t : spec.tracks) {
                if (t < 0 || static_cast<size_t>(t) >= project.tracks.size()) {
                    error = "An output names a track that isn't in the project";
                    return false;
                }
                const Track &track = project.tracks[static_cast<size_t>(t)];
                const bool counted = !spec.respectMuteSolo || (!track.mute && (!anySolo || track.solo));
                if (!counted)
                    continue;
                o->summed.push_back(t);
                if (std::find(tapped.begin(), tapped.end(), t) == tapped.end())
                    tapped.push_back(t);
            }
        }
        o->mix.assign(kBlock * 2, 0.0f);
        length = std::max(length, spec.to + spec.tail);
        outputs.push_back(std::move(o));
    }

    // The engine was prepared for this rate on the main thread before the pass was started
    if (engine_.sampleRate() != rate) {
        error = "The engine isn't prepared at the pass's sample rate";
        return false;
    }
    engine_.beginRender(config_.startBeat, tapped, config_.preFader, config_.unmuted);

    for (auto &o : outputs) {
        o->writer = std::make_unique<audio::FloatTempWriter>();
        if (!o->writer->open(o->spec.path, 2, rate, error)) {
            discard(outputs);
            engine_.endRender();
            return false;
        }
        o->loudness = std::make_unique<audio::master::LoudnessMeter>(2, rate);
        o->truePeak = std::make_unique<audio::master::TruePeakMeter>(2);
    }

    // Each block the master is rendered once; an output reads it or sums the taps it takes
    const uint64_t fifth = rate / 5;
    std::vector<float> master(kBlock * 2);
    uint64_t position = 0; // frames from the pass's start
    bool cancelled = false;
    bool failed = false;
    for (;;) {
        bool allDone = true;
        for (auto &o : outputs)
            allDone = allDone && o->done;
        if (allDone)
            break;
        if (cancel_.load(std::memory_order_relaxed)) {
            cancelled = true;
            break;
        }

        engine_.render(master.data(), kBlock);
        for (auto &o : outputs) {
            if (o->done)
                continue;
            if (o->spec.master) {
                std::copy_n(master.data(), kBlock * 2, o->mix.data());
            } else {
                std::fill(o->mix.begin(), o->mix.end(), 0.0f);
                for (int t : o->summed) {
                    const float *left = engine_.tapLeft(t);
                    const float *right = engine_.tapRight(t);
                    if (!left || !right)
                        continue;
                    for (uint32_t f = 0; f < kBlock; ++f) {
                        o->mix[f * 2] += left[f];
                        o->mix[f * 2 + 1] += right[f];
                    }
                }
            }

            // The frames of this block that the output keeps: its range, then its tail while it rings
            size_t first = 0, count = 0;
            for (size_t f = 0; f < kBlock && !o->done; ++f) {
                const uint64_t g = position + f;
                if (g < o->spec.from)
                    continue;
                const bool inRange = g < o->spec.to;
                if (!inRange && g >= o->spec.to + o->spec.tail) {
                    o->done = true;
                    break;
                }
                if (count == 0)
                    first = f;
                ++count;
                if (!inRange) {
                    const float peak = std::max(std::fabs(o->mix[f * 2]), std::fabs(o->mix[f * 2 + 1]));
                    o->quiet = peak < kQuietLevel ? o->quiet + 1 : 0;
                    if (o->quiet >= fifth)
                        o->done = true;
                }
            }
            if (count == 0)
                continue;

            float *data = o->mix.data() + first * 2;
            if (!o->writer->write(data, count)) {
                error = "Couldn't write " + o->spec.path;
                failed = true;
                break;
            }
            o->loudness->add(data, count);
            o->truePeak->add(data, count);
            for (size_t i = 0; i < count * 2; ++i) {
                const float v = std::fabs(data[i]);
                o->samplePeak = std::max(o->samplePeak, v);
                if (v > 1.0f)
                    ++o->clipped;
            }
            o->frames += count;
        }
        if (failed)
            break;

        position += kBlock;
        rendered_ = position;
        fraction_.store(static_cast<float>(std::min(0.99, static_cast<double>(position) / static_cast<double>(length))),
                        std::memory_order_relaxed);
    }
    engine_.endRender();

    if (cancelled || failed) {
        discard(outputs);
        cancelled_.store(cancelled, std::memory_order_relaxed);
        if (cancelled)
            error = "Cancelled";
        return false;
    }

    for (auto &o : outputs) {
        std::string closeError;
        if (!o->writer->close(closeError)) {
            error = closeError;
            discard(outputs);
            return false;
        }
        o->writer.reset();
        PassResult result;
        result.id = o->spec.id;
        result.path = o->spec.path;
        result.frames = o->frames;
        result.integratedLufs = o->loudness->integrated();
        result.loudnessRange = o->loudness->range();
        result.shortTermMax = o->loudness->shortTermMax();
        result.shortTerm = o->loudness->shortTermSeries(0.5);
        result.truePeakDb = o->truePeak->peakDb();
        result.samplePeak = o->samplePeak;
        result.clipped = o->clipped;
        results_.push_back(std::move(result));
    }
    fraction_.store(1.0f, std::memory_order_relaxed);
    return true;
}

struct RenderPassRun {
    RenderPass pass;
    std::atomic<int> outcome{0}; // 0 while running, then 1 done, 2 failed, 3 cancelled
    std::string error;
    std::vector<PassResult> results;

    RenderPassRun(OfflineEngine &engine, RenderPassConfig config) : pass(engine, std::move(config)) {}
};

namespace {

// Frames the finishing reads from a render's file at a time
constexpr size_t kFinishChunk = 65536;
constexpr double kInfinity = std::numeric_limits<double>::infinity();

// What finishing one audio output needs, copied out when its pass has ended
struct FinishSpec {
    size_t output = 0;
    std::string name;
    std::string temp;
    std::string path;
    std::string note;
    ExportTarget target;
    ExportMetadata metadata;
    std::string projectName;
    uint32_t rate = 48000;
    PassResult measured;
};

double samplePeakDb(float linear)
{
    return linear > 0.0f ? 20.0 * std::log10(static_cast<double>(linear)) : -kInfinity;
}

// The gain in dB a target's normalizing asks for, from what its render measured (0 when it doesn't
// normalize, or there's nothing to measure: silence)
double gainFor(const ExportTarget &t, const PassResult &m)
{
    if (t.normalize == NormalizeMode::Peak) {
        const double measured = t.limiter ? m.truePeakDb : samplePeakDb(m.samplePeak);
        if (std::isfinite(measured) && measured > -150.0)
            return t.peakDb - measured;
    } else if (t.normalize == NormalizeMode::Loudness && std::isfinite(m.integratedLufs)) {
        return t.loudnessLufs - m.integratedLufs;
    }
    return 0.0;
}

audio::master::DitherMode ditherMode(DitherMode mode)
{
    switch (mode) {
    case DitherMode::None: return audio::master::DitherMode::None;
    case DitherMode::Tpdf: return audio::master::DitherMode::Tpdf;
    case DitherMode::NoiseShaped: return audio::master::DitherMode::NoiseShaped;
    }
    return audio::master::DitherMode::Tpdf;
}

std::string todayText()
{
    const std::time_t now = std::time(nullptr);
    const std::tm *local = std::localtime(&now);
    char buffer[16] = {};
    if (local)
        std::strftime(buffer, sizeof buffer, "%Y-%m-%d", local);
    return buffer;
}

std::string rateText(uint32_t rate)
{
    char buffer[32];
    if (rate % 1000 == 0)
        std::snprintf(buffer, sizeof buffer, "%u kHz", rate / 1000);
    else
        std::snprintf(buffer, sizeof buffer, "%.1f kHz", rate / 1000.0);
    return buffer;
}

// Writes a report into its slot and counts the output as finished
void storeReport(RenderResults &results, size_t index, TargetReport report)
{
    {
        std::lock_guard<std::mutex> lock(results.mutex);
        results.reports[index] = std::move(report);
    }
    results.finished.fetch_add(1);
}

// The bits an output's samples are written at: 16, 24, or 32 (float, WAV only)
int outputBits(const ExportTarget &t)
{
    int bits = t.bits == 16 || t.bits == 24 || t.bits == 32 ? t.bits : 24;
    if (t.format != ExportFormat::Wav && bits == 32)
        bits = 24;
    return bits;
}

// What one run through an output's chain measured of the signal that came out
struct ChainRun {
    audio::master::LoudnessMeter loudness;
    audio::master::TruePeakMeter truePeak;
    float samplePeak = 0.0f;
    uint64_t clipped = 0;
    uint64_t frames = 0;

    ChainRun(uint32_t channels, double rate) : loudness(channels, rate), truePeak(channels) {}
};

// Runs the render's file through an output's chain at `gainDb`: gain, fades, the limiter (when it is
// on), mono and dither, measuring what comes out and writing it when there is a writer. Returns false
// with `error` set on a failure or a cancel.
bool runChain(const FinishSpec &s, double gainDb, audio::AudioWriter *writer, const std::atomic<bool> &cancel,
              ChainRun &run, std::string &error)
{
    const ExportTarget &t = s.target;
    const bool limiterOn = t.limiter && t.normalize != NormalizeMode::Off;
    const uint32_t channels = t.mono ? 1 : 2;
    const double rate = static_cast<double>(s.rate);
    const float gain = static_cast<float>(std::pow(10.0, gainDb / 20.0));
    const int bits = outputBits(t);

    audio::FloatTempReader reader;
    if (!reader.open(s.temp, error))
        return false;
    if (reader.channels() != 2) {
        error = "The render's file is damaged";
        return false;
    }
    std::unique_ptr<audio::master::TruePeakLimiter> limiter;
    if (limiterOn)
        limiter = std::make_unique<audio::master::TruePeakLimiter>(2, rate, t.ceilingDbtp);
    audio::master::Dither dither(ditherMode(t.dither), channels, 1);
    const size_t fadeIn = static_cast<size_t>(std::max(0.0, t.fadeInMs) * rate / 1000.0);
    const size_t fadeOut = static_cast<size_t>(std::max(0.0, t.fadeOutMs) * rate / 1000.0);
    const size_t total = static_cast<size_t>(reader.frames());

    // One block of the output: mono, dither, measured, then written
    auto emit = [&](float *data, size_t frames) -> bool {
        if (frames == 0)
            return true;
        if (channels == 1) {
            for (size_t f = 0; f < frames; ++f)
                data[f] = 0.5f * (data[f * 2] + data[f * 2 + 1]);
        }
        if (bits < 32)
            dither.apply(data, frames, channels, bits);
        run.loudness.add(data, frames);
        run.truePeak.add(data, frames);
        for (size_t i = 0; i < frames * channels; ++i) {
            const float v = std::fabs(data[i]);
            run.samplePeak = std::max(run.samplePeak, v);
            if (v > 1.0f)
                ++run.clipped;
        }
        if (writer && !writer->write(data, frames)) {
            error = "Couldn't write " + s.path;
            return false;
        }
        run.frames += frames;
        return true;
    };

    std::vector<float> buffer(kFinishChunk * 2);
    size_t position = 0;
    for (;;) {
        if (cancel.load(std::memory_order_relaxed)) {
            error = "Cancelled";
            return false;
        }
        const uint64_t n = reader.read(buffer.data(), kFinishChunk);
        if (n == 0)
            break;
        const size_t frames = static_cast<size_t>(n);
        for (size_t i = 0; i < frames * 2; ++i)
            buffer[i] *= gain;
        audio::master::applyFade(buffer.data(), frames, 2, fadeIn, fadeOut, total, position);
        position += frames;
        size_t out = frames;
        if (limiter)
            out = limiter->process(buffer.data(), buffer.data(), frames);
        if (!emit(buffer.data(), out))
            return false;
    }
    if (limiter) {
        const size_t rest = limiter->finish(buffer.data());
        if (!emit(buffer.data(), rest))
            return false;
    }
    return true;
}

// Finishes one audio output and measures the file as written. A loudness target with the limiter on
// is corrected for what the limiter takes off: up to three trial runs measure the loudness and move
// the gain by the miss, keeping the best gain, and only then is the file written; the final signal
// is measured in the writing run. The caller removes the temp file once this has returned.
TargetReport finishAudio(const FinishSpec &s, const std::atomic<bool> &cancel)
{
    TargetReport report;
    report.target = s.name;
    report.path = s.path;
    report.note = s.note;
    const ExportTarget &t = s.target;
    const bool limiterOn = t.limiter && t.normalize != NormalizeMode::Off;
    const uint32_t channels = t.mono ? 1 : 2;
    const double rate = static_cast<double>(s.rate);
    std::string error;

    double gainDb = gainFor(t, s.measured);
    if (t.normalize == NormalizeMode::Loudness && limiterOn) {
        double best = gainDb;
        double bestMiss = kInfinity;
        double trial = gainDb;
        for (int iteration = 0; iteration < 3; ++iteration) {
            ChainRun run(channels, rate);
            if (!runChain(s, trial, nullptr, cancel, run, error)) {
                report.error = error;
                return report;
            }
            const double measured = run.loudness.integrated();
            if (!std::isfinite(measured))
                break; // silence: nothing to correct
            const double miss = measured - t.loudnessLufs;
            if (std::fabs(miss) < bestMiss) {
                bestMiss = std::fabs(miss);
                best = trial;
            }
            if (std::fabs(miss) <= 0.2)
                break;
            trial -= miss;
        }
        gainDb = best;
    }
    report.gainDb = gainDb;

    audio::EncodeFormat format;
    format.container = t.format == ExportFormat::Aiff   ? audio::EncodeFormat::Aiff
                       : t.format == ExportFormat::Flac ? audio::EncodeFormat::Flac
                                                        : audio::EncodeFormat::Wav;
    format.bits = outputBits(t);
    format.sampleRate = s.rate;
    format.channels = channels;

    std::unique_ptr<audio::AudioWriter> writer = audio::makeWriter(format);
    audio::AudioMetadata metadata;
    metadata.title = s.metadata.title.empty() ? s.projectName : s.metadata.title;
    metadata.artist = s.metadata.artist;
    metadata.album = s.metadata.album;
    metadata.comment = s.metadata.comment;
    metadata.date = todayText();
    if (!writer || !writer->open(s.path, format, metadata, error)) {
        report.error = error.empty() ? "Couldn't write " + s.path : error;
        return report;
    }

    ChainRun run(channels, rate);
    if (!runChain(s, gainDb, writer.get(), cancel, run, error)) {
        writer.reset(); // removes the part file
        report.error = error;
        return report;
    }
    std::string closeError;
    if (!writer->close(closeError)) {
        report.error = closeError;
        return report;
    }

    report.ok = true;
    report.seconds = static_cast<double>(run.frames) / rate;
    std::error_code ec;
    const auto size = std::filesystem::file_size(std::filesystem::u8path(s.path), ec);
    report.bytes = ec ? 0 : static_cast<uint64_t>(size);
    report.integratedLufs = run.loudness.integrated();
    report.loudnessRange = run.loudness.range();
    report.shortTermMax = run.loudness.shortTermMax();
    for (double v : run.loudness.shortTermSeries(0.5))
        report.shortTerm.push_back(static_cast<float>(v));
    report.truePeakDb = run.truePeak.peakDb();
    report.samplePeakDb = samplePeakDb(run.samplePeak);
    report.clipped = run.clipped;
    return report;
}

// A pass's audio output, finished on a background job
void finishAudioJob(FinishSpec spec, std::shared_ptr<RenderResults> results)
{
    TargetReport report = finishAudio(spec, results->cancel);
    std::error_code ec;
    std::filesystem::remove(std::filesystem::u8path(spec.temp), ec);
    if (!report.ok && report.error.empty())
        report.error = "Failed";
    storeReport(*results, spec.output, std::move(report));
}

// A MIDI output, written from the project's snapshot on a background job
void writeMidiJob(std::shared_ptr<const Project> project, MidiExportOptions options, std::string path,
                  std::string name, double seconds, size_t output, std::shared_ptr<RenderResults> results)
{
    TargetReport report;
    report.target = name;
    report.path = path;
    report.midi = true;
    report.seconds = seconds;
    std::string error;
    if (writeMidiFile(*project, options, path, error)) {
        report.ok = true;
        std::error_code ec;
        const auto size = std::filesystem::file_size(std::filesystem::u8path(path), ec);
        report.bytes = ec ? 0 : static_cast<uint64_t>(size);
    } else {
        report.error = error;
    }
    storeReport(*results, output, std::move(report));
}

// Frames of a file at `beats` from its pass's start
uint64_t framesAt(double beats, double secondsPerBeat, double rate)
{
    const long long frames = std::llround(beats * secondsPerBeat * rate);
    return frames < 0 ? 0 : static_cast<uint64_t>(frames);
}

bool hasContent(const Track &track)
{
    return std::any_of(track.clips.begin(), track.clips.end(),
                       [](const Clip &c) { return c.hasAudio() || !c.notes.empty(); });
}

}

RenderJob::RenderJob(env::Environment &env, plugin::PluginManager *plugins, InstrumentLibrary &library,
                     AudioPool &pool, Project snapshot, ExportPreset preset, float masterGain, std::string outputFolder,
                     uint64_t id)
    : env_(env), plugins_(plugins), library_(library), pool_(pool), snapshot_(std::move(snapshot)),
      preset_(std::move(preset)), masterGain_(masterGain), outputFolder_(std::move(outputFolder)), id_(id),
      enqueued_(std::chrono::steady_clock::now()), results_(std::make_shared<RenderResults>())
{
    if (outputFolder_.empty())
        outputFolder_ = (std::filesystem::u8path(library_.instrumentsFolder()).parent_path() / "Exports").u8string();
}

RenderJob::~RenderJob()
{
    if (!finished()) {
        results_->cancel = true;
        if (run_)
            run_->pass.cancel();
    }
    renderJobs_.wait();
    finalizeJobs_.wait();
    run_.reset();
    engine_.reset(); // on this thread, once no render uses it
    removeTemp();
}

void RenderJob::makeTempFolder()
{
    tempFolder_ = (std::filesystem::temp_directory_path() / std::filesystem::u8path("Vibin export") /
                   std::to_string(id_))
                      .u8string();
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::u8path(tempFolder_), ec);
}

void RenderJob::removeTemp()
{
    if (tempFolder_.empty())
        return;
    std::error_code ec;
    std::filesystem::remove_all(std::filesystem::u8path(tempFolder_), ec);
}

void RenderJob::plan()
{
    const Project &project = snapshot_;
    const std::time_t now = std::time(nullptr);

    // Adds an output with its report slot (the same index); a non-empty error skips the output
    auto addOutput = [this](PlannedOutput output, const std::string &error) {
        TargetReport report;
        report.target = output.name;
        report.path = output.path;
        report.note = output.note;
        report.midi = output.midi;
        report.error = error;
        output.skipped = !error.empty();
        const bool skipped = output.skipped;
        {
            std::lock_guard<std::mutex> lock(results_->mutex);
            results_->reports.push_back(std::move(report));
        }
        outputs_.push_back(std::move(output));
        if (skipped)
            results_->finished.fetch_add(1);
    };

    // The file an output writes, in its target's folder (or the fallback), made if it isn't there
    auto pathFor = [&](const ExportTarget &target, const std::string &track, std::string &error) {
        const std::string folder = target.folder.empty() ? outputFolder_ : target.folder;
        std::error_code ec;
        std::filesystem::create_directories(std::filesystem::u8path(folder), ec);
        if (!std::filesystem::is_directory(std::filesystem::u8path(folder), ec)) {
            error = "Couldn't create the folder " + folder;
            return std::string();
        }
        const std::string file = resolveFileName(target, project, preset_.name, track, now);
        return (std::filesystem::u8path(folder) / std::filesystem::u8path(file)).u8string();
    };

    // Every path this job writes, lower-cased (Windows doesn't tell the case of names apart)
    std::set<std::string> usedPaths;
    auto lowered = [](std::string s) {
        for (char &ch : s)
            ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        return s;
    };
    // The path itself when no earlier output of the job writes to it; else " (2)", " (3)"... before the
    // extension, with the note saying so
    auto uniquePath = [&](const std::string &path, std::string &note) {
        if (path.empty())
            return path;
        const std::filesystem::path first = std::filesystem::u8path(path);
        const std::string stem = first.stem().u8string();
        const std::string extension = first.extension().u8string();
        const std::string folder = first.parent_path().u8string();
        std::string candidate = path;
        int copy = 1;
        while (usedPaths.count(lowered(candidate)) > 0) {
            ++copy;
            candidate = (std::filesystem::u8path(folder) /
                         std::filesystem::u8path(stem + " (" + std::to_string(copy) + ")" + extension))
                            .u8string();
        }
        if (copy > 1) {
            note = "The name \"" + first.filename().u8string() + "\" was taken by another output of this job, so this one is written as \"" +
                   std::filesystem::u8path(candidate).filename().u8string() + "\"";
        }
        usedPaths.insert(lowered(candidate));
        return candidate;
    };

    for (const ExportTarget &target : preset_.targets) {
        if (!target.enabled)
            continue;

        // The outputs a target makes: the master, the chosen tracks summed, or one per track
        struct Choice {
            std::string track;
            std::vector<int> tracks;
            bool master;
        };
        std::vector<Choice> choices;
        std::string noChoice;
        std::vector<int> chosen;
        for (const std::string &name : target.tracks) {
            for (size_t i = 0; i < project.tracks.size(); ++i) {
                if (project.tracks[i].name == name) {
                    chosen.push_back(static_cast<int>(i));
                    break;
                }
            }
        }
        switch (target.source) {
        case ExportSource::Master:
            choices.push_back({"", {}, true});
            break;
        case ExportSource::MasterDry: {
            std::vector<int> all;
            for (size_t i = 0; i < project.tracks.size(); ++i)
                all.push_back(static_cast<int>(i));
            choices.push_back({"", all, false});
            break;
        }
        case ExportSource::EachTrack:
            for (size_t i = 0; i < project.tracks.size(); ++i) {
                if (hasContent(project.tracks[i]))
                    choices.push_back({project.tracks[i].name, {static_cast<int>(i)}, false});
            }
            if (choices.empty())
                noChoice = "No track has anything to export";
            break;
        case ExportSource::Tracks:
            if (chosen.empty())
                noChoice = "None of the chosen tracks is in the project";
            else
                choices.push_back({"", chosen, false});
            break;
        case ExportSource::Midi:
            // Empty: every instrument track with notes (the MIDI writer's choice)
            if (!target.tracks.empty() && chosen.empty())
                noChoice = "None of the chosen tracks is in the project";
            else
                choices.push_back({"", chosen, false});
            break;
        }
        if (choices.empty() && noChoice.empty())
            noChoice = "Nothing to export";

        if (!noChoice.empty()) {
            PlannedOutput output;
            output.target = target;
            output.name = target.name;
            output.midi = target.source == ExportSource::Midi;
            addOutput(std::move(output), noChoice);
            continue;
        }

        for (const Choice &choice : choices) {
            PlannedOutput output;
            output.target = target;
            output.midi = target.source == ExportSource::Midi;
            output.master = choice.master;
            output.tracks = choice.tracks;
            output.name = choice.track.empty() ? target.name : target.name + " - " + choice.track;
            std::string error;
            double startBeat = 0.0, endBeat = 0.0;
            if (!exportTargetRange(project, target, startBeat, endBeat))
                error = "The target's range is empty";
            output.startBeat = startBeat;
            output.endBeat = endBeat;
            if (error.empty())
                output.path = uniquePath(pathFor(target, choice.track, error), output.note);
            addOutput(std::move(output), error);
        }
    }

    // Outputs of one rate, tap and mute rule share a pass; a pass starts at the earliest of them
    for (size_t i = 0; i < outputs_.size(); ++i) {
        PlannedOutput &o = outputs_[i];
        if (o.skipped || o.midi)
            continue;
        o.tempPath = (std::filesystem::u8path(tempFolder_) / ("output-" + std::to_string(i) + ".f32")).u8string();
        const uint32_t rate = o.target.sampleRate;
        const bool preFader = o.target.tap == ExportTap::PreFader;
        const bool unmuted = !o.master && !o.target.respectMuteSolo;
        size_t pass = passes_.size();
        for (size_t p = 0; p < passes_.size(); ++p) {
            if (passes_[p].rate == rate && passes_[p].preFader == preFader && passes_[p].unmuted == unmuted) {
                pass = p;
                break;
            }
        }
        if (pass == passes_.size()) {
            PlannedPass made;
            made.rate = rate;
            made.preFader = preFader;
            made.unmuted = unmuted;
            made.startBeat = o.startBeat;
            passes_.push_back(made);
        }
        passes_[pass].startBeat = std::min(passes_[pass].startBeat, o.startBeat);
        passes_[pass].outputs.push_back(i);
        o.pass = static_cast<int>(pass);
    }

    // Each output's range and tail, in frames from its pass's start
    const double secondsPerBeat = project.secondsPerBeat();
    for (PlannedOutput &o : outputs_) {
        if (o.skipped || o.midi)
            continue;
        const PlannedPass &pass = passes_[static_cast<size_t>(o.pass)];
        const double rate = static_cast<double>(pass.rate);
        o.from = framesAt(o.startBeat - pass.startBeat, secondsPerBeat, rate);
        o.to = framesAt(o.endBeat - pass.startBeat, secondsPerBeat, rate);
        o.tail = framesAt(std::max(0.0, o.target.tailSeconds) / secondsPerBeat, secondsPerBeat, rate);
    }

    for (size_t i = 0; i < outputs_.size(); ++i) {
        if (outputs_[i].midi && !outputs_[i].skipped)
            startMidi(i);
    }
}

void RenderJob::startPass()
{
    const PlannedPass &pass = passes_[nextPass_];
    RenderPassConfig config;
    config.sampleRate = pass.rate;
    config.startBeat = pass.startBeat;
    config.preFader = pass.preFader;
    config.unmuted = pass.unmuted;
    for (size_t index : pass.outputs) {
        const PlannedOutput &o = outputs_[index];
        PassOutput spec;
        spec.id = static_cast<int>(index);
        spec.master = o.master;
        spec.tracks = o.tracks;
        spec.respectMuteSolo = o.target.respectMuteSolo;
        spec.from = o.from;
        spec.to = o.to;
        spec.tail = o.tail;
        spec.path = o.tempPath;
        config.outputs.push_back(std::move(spec));
    }

    run_ = std::make_shared<RenderPassRun>(*engine_, std::move(config));
    passRunning_ = true;
    state_ = Rendering;
    status_ = "Rendering " + preset_.name + " at " + rateText(pass.rate) + "...";
    std::shared_ptr<RenderPassRun> run = run_;
    env::runJob(env_, env::Lane::Background,
                [run] {
                    std::string error;
                    const bool ok = run->pass.run(error);
                    if (ok)
                        run->results = run->pass.results();
                    run->error = error;
                    run->outcome.store(ok ? 1 : run->pass.cancelled() ? 3 : 2, std::memory_order_release);
                },
                renderJobs_);
}

void RenderJob::collectPass()
{
    const int outcome = run_->outcome.load(std::memory_order_acquire);
    const PlannedPass &pass = passes_[nextPass_];
    for (size_t index : pass.outputs) {
        if (outcome != 1) {
            failOutput(index, outcome == 3 ? "Cancelled" : run_->error);
            continue;
        }
        const PassResult *result = nullptr;
        for (const PassResult &r : run_->results) {
            if (r.id == static_cast<int>(index))
                result = &r;
        }
        if (!result)
            failOutput(index, "The render produced no result");
        else
            startFinalize(index, *result);
    }
    passRunning_ = false;
    run_.reset();
    ++nextPass_;
}

void RenderJob::startFinalize(size_t index, const PassResult &result)
{
    const PlannedOutput &o = outputs_[index];
    FinishSpec spec;
    spec.output = index;
    spec.name = o.name;
    spec.temp = o.tempPath;
    spec.path = o.path;
    spec.note = o.note;
    spec.target = o.target;
    spec.metadata = snapshot_.exportMetadata;
    spec.projectName = snapshot_.name.empty() ? std::string("Untitled") : snapshot_.name;
    spec.rate = passes_[static_cast<size_t>(o.pass)].rate;
    spec.measured = result;
    status_ = "Finishing " + o.name + "...";
    std::shared_ptr<RenderResults> results = results_;
    env::runJob(env_, env::Lane::Background,
                [spec = std::move(spec), results]() mutable { finishAudioJob(std::move(spec), results); },
                finalizeJobs_);
}

void RenderJob::startMidi(size_t index)
{
    const PlannedOutput &o = outputs_[index];
    auto project = std::make_shared<const Project>(snapshot_);
    MidiExportOptions options;
    options.tracks = o.tracks;
    options.fromBeat = o.startBeat;
    options.toBeat = o.endBeat;
    const double seconds = (o.endBeat - o.startBeat) * snapshot_.secondsPerBeat();
    status_ = "Finishing " + o.name + " MIDI";
    std::shared_ptr<RenderResults> results = results_;
    env::runJob(env_, env::Lane::Background,
                [project, options, path = o.path, name = o.name, seconds, index, results] {
                    writeMidiJob(project, options, path, name, seconds, index, results);
                },
                finalizeJobs_);
}

void RenderJob::failOutput(size_t index, const std::string &error)
{
    {
        std::lock_guard<std::mutex> lock(results_->mutex);
        results_->reports[index].ok = false;
        results_->reports[index].error = error;
    }
    results_->finished.fetch_add(1);
}

bool RenderJob::anyFailed() const
{
    std::lock_guard<std::mutex> lock(results_->mutex);
    for (const TargetReport &r : results_->reports) {
        if (!r.ok)
            return true;
    }
    return false;
}

void RenderJob::finish(State state)
{
    state_ = state;
    finishedAt_ = std::chrono::steady_clock::now();
    run_.reset();
    engine_.reset(); // no render uses it now: every job has let go
    removeTemp();
    if (state == Done) {
        status_ = "Done";
    } else if (state == Cancelled) {
        status_ = "Cancelled";
    } else {
        status_ = "Failed";
        std::lock_guard<std::mutex> lock(results_->mutex);
        for (const TargetReport &r : results_->reports) {
            if (!r.ok && !r.error.empty()) {
                status_ += ": " + r.error;
                break;
            }
        }
    }
}

void RenderJob::update()
{
    if (finished())
        return;
    if (!started_) {
        started_ = true;
        startedAt_ = std::chrono::steady_clock::now();
        makeTempFolder();
        plan();
    }
    if (cancelRequested_) {
        if (renderJobs_.pending() == 0 && finalizeJobs_.pending() == 0)
            finish(Cancelled);
        return;
    }
    if (nextPass_ < passes_.size()) {
        if (passRunning_) {
            if (run_->outcome.load(std::memory_order_acquire) != 0)
                collectPass();
            return;
        }

        // The engine is prepared on this thread for the next pass, and only then handed to its render
        const PlannedPass &pass = passes_[nextPass_];
        state_ = Preparing;
        try {
            if (!engine_) {
                engine_ = std::make_unique<OfflineEngine>(env_, plugins_, library_, pool_, snapshot_, masterGain_,
                                                          pass.rate);
            } else if (engine_->sampleRate() != pass.rate) {
                engine_->setRate(pass.rate);
            }
            std::string waiting;
            if (!engine_->prepare(waiting)) {
                status_ = waiting.empty() ? std::string("Preparing...") : "Preparing " + waiting + "...";
                return;
            }
            startPass();
        } catch (const std::exception &e) {
            for (size_t p = nextPass_; p < passes_.size(); ++p) {
                for (size_t index : passes_[p].outputs)
                    failOutput(index, e.what());
            }
            nextPass_ = passes_.size();
        }
        return;
    }

    state_ = Finalizing;
    if (renderJobs_.pending() == 0 && finalizeJobs_.pending() == 0)
        finish(anyFailed() ? Failed : Done);
}

void RenderJob::cancel()
{
    if (finished())
        return;
    if (!started_) {
        state_ = Cancelled;
        status_ = "Cancelled";
        finishedAt_ = std::chrono::steady_clock::now();
        return;
    }
    cancelRequested_ = true;
    results_->cancel = true;
    if (run_)
        run_->pass.cancel();
    status_ = "Cancelling...";
}

float RenderJob::fraction() const
{
    if (finished())
        return 1.0f;
    if (!started_)
        return 0.0f;
    float render = passes_.empty() ? 1.0f : static_cast<float>(nextPass_) / static_cast<float>(passes_.size());
    if (passRunning_ && run_)
        render += run_->pass.fraction() / static_cast<float>(passes_.size());
    render = std::min(1.0f, render);
    const float finishing = outputs_.empty() ? 1.0f
                                             : static_cast<float>(results_->finished.load()) /
                                                   static_cast<float>(outputs_.size());
    return 0.8f * render + 0.2f * std::min(1.0f, finishing);
}

std::vector<TargetReport> RenderJob::reports() const
{
    std::lock_guard<std::mutex> lock(results_->mutex);
    return results_->reports;
}

double RenderJob::elapsedSeconds() const
{
    if (!started_)
        return 0.0;
    const auto end = finished() ? finishedAt_ : std::chrono::steady_clock::now();
    return std::chrono::duration<double>(end - startedAt_).count();
}

ExportQueue::ExportQueue(env::Environment &env, plugin::PluginManager *plugins, InstrumentLibrary &library,
                         AudioPool &pool)
    : env_(env), plugins_(plugins), library_(library), pool_(pool)
{
}

ExportQueue::~ExportQueue() = default;

uint64_t ExportQueue::enqueue(Project snapshot, ExportPreset preset, float masterGain, std::string outputFolder)
{
    const uint64_t id = nextId_++;
    jobs_.push_back(std::make_unique<RenderJob>(env_, plugins_, library_, pool_, std::move(snapshot),
                                                std::move(preset), masterGain, std::move(outputFolder), id));
    return id;
}

std::vector<RenderJob *> ExportQueue::jobs() const
{
    std::vector<RenderJob *> out;
    for (const auto &job : jobs_)
        out.push_back(job.get());
    return out;
}

RenderJob *ExportQueue::active() const
{
    for (const auto &job : jobs_) {
        if (!job->finished())
            return job.get();
    }
    return nullptr;
}

void ExportQueue::update()
{
    if (RenderJob *job = active())
        job->update();
}

bool ExportQueue::cancel(uint64_t id)
{
    for (const auto &job : jobs_) {
        if (job->id() == id) {
            job->cancel();
            return true;
        }
    }
    return false;
}

bool ExportQueue::remove(uint64_t id)
{
    for (auto it = jobs_.begin(); it != jobs_.end(); ++it) {
        if ((*it)->id() != id)
            continue;
        if (!(*it)->finished())
            return false;
        jobs_.erase(it);
        return true;
    }
    return false;
}

}
