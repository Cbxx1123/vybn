#include "EngineSelfTest.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#include "audio/AudioFile.h"
#include "audio/AudioSystem.h"
#include "audio/ClipPlayer.h"
#include "audio/Memory.h"
#include "audio/Preview.h"
#include "audio/Processors.h"
#include "audio/SampleSet.h"
#include "audio/Sampler.h"
#include "audio/Track.h"
#include "env/Environment.h"
#include "env/MpmcQueue.h"
#include "env/RealtimePool.h"
#include "env/Resources.h"
#include "env/Scheduler.h"

namespace tests {

namespace {

namespace fs = std::filesystem;

struct Checker {
    int failures = 0;

    void check(bool ok, const std::string &what, const std::string &detail = {})
    {
        std::printf("%s  %-54s %s\n", ok ? "PASS" : "FAIL", what.c_str(), detail.c_str());
        if (!ok)
            ++failures;
    }
};

std::string text(const char *pattern, double value)
{
    char buffer[128];
    std::snprintf(buffer, sizeof(buffer), pattern, value);
    return buffer;
}

double seconds(std::chrono::steady_clock::time_point since)
{
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - since).count();
}

// Tracks of tones mixed for `frames`, with the scheduler's pool (if any) using `workers`
std::vector<float> mixTones(bool scheduler, uint32_t workers, uint32_t tracks, uint32_t frames, double &took)
{
    env::Environment environment;
    if (scheduler) {
        env::SchedulerConfig config;
        config.realtimeWorkers = 4;
        environment.add<env::Scheduler>(config).setRealtimeWorkers(workers);
    }
    auto &system = environment.add<audio::AudioSystem>();
    system.masterGain().set(1.0f);
    auto solo = std::make_shared<audio::SoloGroup>();
    for (uint32_t t = 0; t < tracks; ++t) {
        auto tone = std::make_unique<audio::ToneGenerator>();
        tone->frequency.set(110.0f * static_cast<float>(t + 1));
        tone->gain.set(0.5f / static_cast<float>(tracks));
        tone->enabled = true;
        auto &track = system.add<audio::TrackProcessor>("Track", solo, std::move(tone));
        track.pan.set(static_cast<float>(t) / static_cast<float>(tracks) * 2.0f - 1.0f);
    }
    std::vector<float> out(static_cast<size_t>(frames) * 2);
    const auto begin = std::chrono::steady_clock::now();
    for (uint32_t done = 0; done < frames; done += 256)
        system.renderOffline(out.data() + static_cast<size_t>(done) * 2, std::min<uint32_t>(256, frames - done), 48000.0);
    took = seconds(begin);
    return out;
}

// Renders a note on a sampler through the audio system, a block at a time, calling `between`
// after each block (serving the disk)
template <typename Between>
std::vector<float> playNote(audio::AudioSystem &system, audio::Sampler &sampler, int key, uint32_t frames, Between between)
{
    std::vector<float> block(512), out;
    system.renderOffline(block.data(), 16, 48000.0); // the set goes in
    sampler.noteOn(key, 0.9f);
    for (uint32_t done = 0; done < frames; done += 256) {
        system.renderOffline(block.data(), 256, 48000.0);
        out.insert(out.end(), block.begin(), block.end());
        between();
    }
    sampler.allNotesOff();
    for (int i = 0; i < 40; ++i) {
        system.renderOffline(block.data(), 256, 48000.0);
        between();
    }
    return out;
}

}

int runEngineSelfTest()
{
    std::printf("Engine self-test: scheduling, parallel audio and sample memory (%u cores)\n\n", env::Scheduler::cores());
    Checker c;

    // The MPMC queue: four producers, four consumers, every item once
    {
        env::MpmcQueue<uint32_t, 1024> queue;
        constexpr uint32_t kEach = 50000;
        std::atomic<uint64_t> sum{0};
        std::atomic<uint32_t> popped{0};
        std::vector<std::thread> threads;
        for (uint32_t p = 0; p < 4; ++p) {
            threads.emplace_back([&queue, p] {
                for (uint32_t i = 0; i < kEach; ++i) {
                    while (!queue.push(p * kEach + i + 1))
                        std::this_thread::yield();
                }
            });
        }
        for (uint32_t q = 0; q < 4; ++q) {
            threads.emplace_back([&] {
                uint32_t v;
                while (popped.load() < 4 * kEach) {
                    if (queue.pop(v)) {
                        sum += v;
                        ++popped;
                    } else {
                        std::this_thread::yield();
                    }
                }
            });
        }
        for (auto &t : threads)
            t.join();
        const uint64_t n = 4ull * kEach;
        c.check(popped == n && sum == n * (n + 1) / 2, "MPMC queue: every item exactly once, 4 by 4 threads",
                text("%.0f items", static_cast<double>(popped.load())));
        uint32_t v;
        int pushed = 0;
        while (queue.push(1))
            ++pushed;
        c.check(pushed == 1024 && queue.pop(v) && v == 1, "MPMC queue: holds its capacity, then refuses");
    }

    // The real-time pool: batch after batch, every job once
    {
        env::RealtimePool pool;
        pool.start(3);
        struct Context {
            std::vector<std::atomic<int>> hits;
            explicit Context(size_t n) : hits(n) {}
        } context(37);
        bool exact = true;
        for (int batch = 0; batch < 2000; ++batch) {
            for (auto &h : context.hits)
                h = 0;
            pool.run(37, [](void *ctx, uint32_t i) { static_cast<Context *>(ctx)->hits[i].fetch_add(1); }, &context);
            for (auto &h : context.hits)
                exact = exact && h.load() == 1;
        }
        c.check(exact, "Real-time pool: 2000 batches, every job run once", text("%.0f run by workers", static_cast<double>(pool.stolen())));
        // One job, and more workers than jobs
        std::atomic<int> one{0};
        pool.run(1, [](void *ctx, uint32_t) { static_cast<std::atomic<int> *>(ctx)->fetch_add(1); }, &one);
        pool.run(2, [](void *ctx, uint32_t) { static_cast<std::atomic<int> *>(ctx)->fetch_add(1); }, &one);
        c.check(one == 3, "Real-time pool: batches smaller than the pool");
        pool.setActive(0);
        std::atomic<int> alone{0};
        pool.run(8, [](void *ctx, uint32_t) { static_cast<std::atomic<int> *>(ctx)->fetch_add(1); }, &alone);
        c.check(alone == 8, "Real-time pool: with no workers the caller runs them all");
        pool.stop();
    }

    // The scheduler: lanes, groups, pollers woken
    {
        env::Environment environment;
        env::SchedulerConfig config;
        config.realtimeWorkers = 1;
        auto &scheduler = environment.add<env::Scheduler>(config);
        std::atomic<int> ran{0};
        {
            env::JobGroup group;
            for (int i = 0; i < 200; ++i) {
                const env::Lane lane = static_cast<env::Lane>(i % 4);
                scheduler.submit(lane, [&ran] { ++ran; }, &group);
            }
            group.wait();
        }
        c.check(ran == 200, "Scheduler: 200 jobs on four lanes, waited for as a group");
        std::atomic<int> polls{0};
        std::atomic<bool> pending{false};
        const int id = scheduler.addPoller(env::Lane::Disk, [&] {
            if (!pending.exchange(false))
                return false;
            ++polls;
            return true;
        }, 1000);
        const auto begin = std::chrono::steady_clock::now();
        pending = true;
        scheduler.wake(env::Lane::Disk);
        while (polls == 0 && seconds(begin) < 2.0)
            std::this_thread::yield();
        const double latency = seconds(begin);
        scheduler.removePoller(id);
        c.check(polls == 1 && latency < 0.5, "Scheduler: a wake runs the Disk lane's poller", text("%.2f ms", latency * 1000.0));
        env::JobGroup late;
        scheduler.stop(environment);
        scheduler.submit(env::Lane::Background, [&ran] { ++ran; }, &late);
        late.wait();
        c.check(ran == 200, "Scheduler: once stopped, jobs are dropped, not waited for");
    }

    // Tracks mixed in parallel sound exactly as on one thread
    {
        double serialTime = 0.0, parallelTime = 0.0;
        const std::vector<float> serial = mixTones(false, 0, 24, 48000, serialTime);
        const std::vector<float> parallel = mixTones(true, 4, 24, 48000, parallelTime);
        float worst = 0.0f, peak = 0.0f;
        for (size_t i = 0; i < serial.size(); ++i) {
            worst = std::max(worst, std::fabs(serial[i] - parallel[i]));
            peak = std::max(peak, std::fabs(serial[i]));
        }
        c.check(worst == 0.0f && peak > 0.1f, "Parallel mixing: 24 tracks across 4 workers match one thread",
                text("%.1f ms on one thread, ", serialTime * 1000.0) + text("%.1f ms in parallel", parallelTime * 1000.0));
    }

    // Streaming a sample from disk
    {
        const fs::path folder = fs::temp_directory_path() / "vibin-engine-selftest";
        std::error_code ec;
        fs::remove_all(folder, ec);
        fs::create_directories(folder / "Instrument", ec);
        // A 4-second stereo note, a decaying chord, so it isn't trimmed early
        const uint32_t frames = 4 * 44100;
        std::vector<float> wave(static_cast<size_t>(frames) * 2);
        for (uint32_t i = 0; i < frames; ++i) {
            const float t = static_cast<float>(i) / 44100.0f;
            const float env = std::exp(-t * 0.6f) * 0.6f;
            wave[i * 2] = env * std::sin(2.0f * 3.14159265f * 261.63f * t);
            wave[i * 2 + 1] = env * std::sin(2.0f * 3.14159265f * 329.63f * t + 0.3f);
        }
        std::string error;
        const std::string instrument = (folder / "Instrument").u8string();
        const bool wrote = audio::writeWavFile((folder / "Instrument" / "C4.wav").u8string(), wave.data(), frames, 2, 44100.0,
                                               error, 16);

        env::Environment environment;
        env::SchedulerConfig config;
        config.realtimeWorkers = 1;
        auto &scheduler = environment.add<env::Scheduler>(config);
        auto &system = environment.add<audio::AudioSystem>();
        system.masterGain().set(1.0f);
        audio::MemoryConfig memoryConfig;
        memoryConfig.preloadFrames = 8192;
        memoryConfig.chunkFrames = 4096;
        memoryConfig.budgetBytes = 8 * 4096 * 2 * 2; // eight chunks: the note needs more, so it evicts
        memoryConfig.cacheFolder = (folder / "cache").u8string();
        auto &memory = environment.add<audio::MemoryManager>(memoryConfig);
        auto &sampler = system.add<audio::Sampler>("Streamed");
        (void)scheduler;

        auto whole = wrote ? audio::loadInstrumentFolder(instrument, error) : nullptr;
        auto streamed = wrote ? audio::loadInstrumentFolder(instrument, error, &memory) : nullptr;
        const bool isStreamed = streamed && streamed->zones.size() == 1 && streamed->zones[0].stream;
        c.check(whole && isStreamed && streamed->bytes() == 8192ull * 2 * 2 && whole->bytes() > 10 * streamed->bytes(),
                "Streaming: a long sample keeps only its preload in RAM",
                whole && streamed ? text("%.0f KB in RAM ", streamed->bytes() / 1024.0) + text("of %.0f KB", whole->bytes() / 1024.0)
                                  : error);

        // The same audio as the sample loaded whole, with the disk served between blocks
        std::vector<float> reference, played;
        if (whole && isStreamed) {
            sampler.setSampleSet(whole);
            reference = playNote(system, sampler, 60, frames - 4096, [] {});
            sampler.setSampleSet(streamed);
            played = playNote(system, sampler, 60, frames - 4096, [&memory] { memory.poll(); });
        }
        float worst = 1.0f;
        if (!reference.empty() && reference.size() == played.size()) {
            worst = 0.0f;
            for (size_t i = 0; i < reference.size(); ++i)
                worst = std::max(worst, std::fabs(reference[i] - played[i]));
        }
        const audio::MemoryManager::Stats afterPlay = memory.stats();
        c.check(worst == 0.0f && afterPlay.underruns == 0 && afterPlay.reads > 30,
                "Streaming: plays exactly as loaded whole, chunks arriving in time",
                text("%.0f chunks read", static_cast<double>(afterPlay.reads)) +
                    text(", %.0f underruns", static_cast<double>(afterPlay.underruns)));
        c.check(afterPlay.evictions > 0 && afterPlay.poolBytes <= memoryConfig.budgetBytes,
                "Streaming: the chunk pool stays inside its budget, evicting",
                text("%.0f evicted", static_cast<double>(afterPlay.evictions)) + text(", pool %.0f KB", afterPlay.poolBytes / 1024.0));

        // Without the disk the voice goes quiet after its preload, and keeps time
        std::vector<float> starved;
        if (isStreamed)
            starved = playNote(system, sampler, 60, 30000, [] {});
        float preloadPeak = 0.0f, laterPeak = 0.0f;
        for (size_t i = 0; i + 1 < starved.size(); ++i) {
            const size_t frame = i / 2;
            if (frame < 7000)
                preloadPeak = std::max(preloadPeak, std::fabs(starved[i]));
            else if (frame > 26000)
                laterPeak = std::max(laterPeak, std::fabs(starved[i]));
        }
        c.check(preloadPeak > 0.1f && laterPeak < 1e-6f && memory.stats().underruns > afterPlay.underruns,
                "Streaming: with no disk reads, silence after the preload",
                text("%.0f underruns", static_cast<double>(memory.stats().underruns - afterPlay.underruns)));
        memory.poll(); // what the starved voice asked for

        // A second load reads the cache: the same audio, without decoding
        std::shared_ptr<audio::SampleSet> again;
        if (isStreamed) {
            const auto begin = std::chrono::steady_clock::now();
            again = audio::loadInstrumentFolder(instrument, error, &memory);
            const double took = seconds(begin);
            c.check(again && again->zones[0].stream && again->zones[0].frames == streamed->zones[0].frames &&
                        *again->zones[0].pcm == *streamed->zones[0].pcm,
                    "Streaming: loading again reads the sample cache", text("%.1f ms", took * 1000.0));
        }

        // The disk lane serving it on its own threads, while the audio runs
        uint64_t threadedUnderruns = 0;
        if (isStreamed) {
            // About twice real time: 256 frames are 5.3 ms of audio
            memory.start(environment); // the poller joins the Disk lane
            const uint64_t before = memory.stats().underruns;
            played = playNote(system, sampler, 60, frames - 4096,
                              [] { std::this_thread::sleep_for(std::chrono::microseconds(2500)); });
            threadedUnderruns = memory.stats().underruns - before;
            worst = 0.0f;
            for (size_t i = 0; i < reference.size() && i < played.size(); ++i)
                worst = std::max(worst, std::fabs(reference[i] - played[i]));
            memory.stop(environment);
            c.check(worst == 0.0f && threadedUnderruns == 0, "Streaming: the Disk lane's threads keep up",
                    text("max difference %.6f", worst) + text(", %.0f underruns", static_cast<double>(threadedUnderruns)));
        }

        // Offline (exporting), as fast as it renders, with nothing serving the disk: chunks are
        // read as they're needed
        if (isStreamed) {
            memory.setOffline(true);
            const uint64_t before = memory.stats().underruns;
            played = playNote(system, sampler, 60, frames - 4096, [] {});
            memory.setOffline(false);
            worst = 0.0f;
            for (size_t i = 0; i < reference.size() && i < played.size(); ++i)
                worst = std::max(worst, std::fabs(reference[i] - played[i]));
            const uint64_t underruns = memory.stats().underruns - before;
            c.check(worst == 0.0f && underruns == 0, "Streaming: offline renders read chunks as they need them",
                    text("max difference %.6f", worst) + text(", %.0f underruns", static_cast<double>(underruns)));
        }

        // Sources go once nothing holds them
        sampler.setSampleSet(nullptr);
        std::vector<float> block(512);
        for (int i = 0; i < 4; ++i)
            system.renderOffline(block.data(), 64, 48000.0);
        sampler.collect();
        whole.reset();
        streamed.reset();
        again.reset();
        memory.poll();
        c.check(memory.stats().sources == 0 && memory.stats().preloadBytes == 0, "Streaming: released sources are let go",
                text("%.0f left", memory.stats().sources));
        system.remove(sampler);
        system.renderOffline(block.data(), 16, 48000.0);
        fs::remove_all(folder, ec);
    }

    // Streamed audio clips: a long file decoded a piece at a time into the cache's float copy,
    // read back from it, and played and exported exactly as the file loaded whole
    {
        const fs::path folder = fs::temp_directory_path() / "vibin-engine-clip-selftest";
        std::error_code ec;
        fs::remove_all(folder, ec);
        fs::create_directories(folder, ec);
        // 8 s of stereo at 48 kHz in 24 bits, so it has to be decoded
        const uint32_t frames = 8 * 48000;
        std::vector<float> wave(static_cast<size_t>(frames) * 2);
        for (uint32_t i = 0; i < frames; ++i) {
            const float t = static_cast<float>(i) / 48000.0f;
            wave[i * 2] = 0.5f * std::sin(2.0f * 3.14159265f * (200.0f + 40.0f * t) * t);
            wave[i * 2 + 1] = 0.4f * std::cos(2.0f * 3.14159265f * 310.0f * t);
        }
        std::string error;
        const std::string wav = (folder / "Long take.wav").u8string();
        const bool wrote = audio::writeWavFile(wav, wave.data(), frames, 2, 48000.0, error, 24);

        env::Environment environment;
        env::SchedulerConfig config;
        config.realtimeWorkers = 1;
        environment.add<env::Scheduler>(config);
        auto &system = environment.add<audio::AudioSystem>();
        system.masterGain().set(1.0f);
        audio::MemoryConfig memoryConfig;
        memoryConfig.preloadFrames = 8192;
        memoryConfig.chunkFrames = 4096; // 2048 frames of floats
        memoryConfig.budgetBytes = 16 * 4096 * 2 * 2; // sixteen chunks, about 0.7 s: it evicts
        memoryConfig.cacheFolder = (folder / "cache").u8string();
        auto &memory = environment.add<audio::MemoryManager>(memoryConfig);

        auto whole = wrote ? audio::loadAudioFile(wav, error) : nullptr;
        auto streamed = wrote ? audio::loadAudioFile(wav, error, &memory) : nullptr;
        const bool isStreamed = whole && streamed && streamed->streamed() && streamed->samples.empty() &&
                                streamed->frames == whole->frames && streamed->channels == 2;
        c.check(isStreamed && streamed->peaks == whole->peaks &&
                    streamed->bytes() == (8192 * 2 + whole->peaks.size()) * sizeof(float) &&
                    whole->bytes() > 20 * streamed->bytes(),
                "Clips: a long file keeps its preload and overview in RAM",
                streamed && whole ? text("%.0f KB in RAM ", streamed->bytes() / 1024.0) + text("of %.0f KB", whole->bytes() / 1024.0)
                                  : error);
        std::shared_ptr<audio::AudioData> again;
        if (isStreamed) {
            streamed.reset();
            const auto begin = std::chrono::steady_clock::now();
            again = audio::loadAudioFile(wav, error, &memory);
            const double took = seconds(begin);
            c.check(again && again->streamed() && again->frames == whole->frames && again->peaks == whole->peaks,
                    "Clips: loading again reads the cache's float copy", text("%.1f ms", took * 1000.0));
        }

        // On a track, as a project plays them: one from 1.25 s into the file with a fade in, one
        // from 3 s overlapping its last beat (a crossfade)
        auto solo = std::make_shared<audio::SoloGroup>();
        auto owned = std::make_unique<audio::AudioClipPlayer>();
        audio::AudioClipPlayer *clips = owned.get();
        system.add<audio::TrackProcessor>("Clips", solo, std::move(owned));
        auto setClips = [&](std::shared_ptr<const audio::AudioData> data) {
            auto sequence = std::make_unique<audio::ClipSequence>();
            if (data) {
                audio::ClipRegion a, b;
                a.data = b.data = data;
                a.start = 0.0;
                a.end = 6.0;
                a.offset = 1.25;
                a.fadeIn = 0.1;
                b.start = 5.0;
                b.end = 12.0;
                b.offset = 3.0;
                b.gain = 0.7f;
                sequence->regions = {a, b};
                sequence->sort();
                sequence->crossfade(120.0);
            }
            clips->setSequence(std::move(sequence));
            std::vector<float> block(64);
            system.renderOffline(block.data(), 16, 48000.0);
            clips->collect();
        };
        // Six seconds rendered offline from the start, as an export renders them: a quarter second
        // stopped first, so the clips' first chunks are asked for and the levels settle
        auto exportWith = [&](std::shared_ptr<const audio::AudioData> data) {
            setClips(data);
            std::vector<float> out;
            std::vector<float> block(2 * 4096);
            system.transport().stop();
            system.transport().locate(0);
            for (int settle = 0; settle < 3; ++settle)
                system.renderOffline(block.data(), 4096, 48000.0);
            system.transport().play();
            for (uint64_t done = 0; done < 6 * 48000; done += 4096) {
                system.renderOffline(block.data(), 4096, 48000.0);
                out.insert(out.end(), block.begin(), block.end());
            }
            system.transport().stop();
            setClips(nullptr);
            return out;
        };
        auto difference = [](const std::vector<float> &a, const std::vector<float> &b, float &loudest) {
            loudest = 0.0f;
            if (a.size() != b.size() || a.empty())
                return 1.0f;
            float worst = 0.0f;
            for (size_t i = 0; i < a.size(); ++i) {
                worst = std::max(worst, std::fabs(a[i] - b[i]));
                loudest = std::max(loudest, std::fabs(a[i]));
            }
            return worst;
        };

        // Exporting: offline, nothing serving the disk, every chunk read as the render needs it
        if (again) {
            const std::vector<float> reference = exportWith(whole);
            memory.setOffline(true);
            const audio::MemoryManager::Stats before = memory.stats();
            const std::vector<float> exported = exportWith(again);
            memory.setOffline(false);
            const audio::MemoryManager::Stats after = memory.stats();
            float loudest = 0.0f;
            const float worst = difference(reference, exported, loudest);
            c.check(worst == 0.0f && loudest > 0.3f && after.underruns == before.underruns && after.reads > before.reads &&
                        after.poolBytes <= memoryConfig.budgetBytes,
                    "Clips: an export is identical to the file in memory",
                    text("%.0f chunks read", static_cast<double>(after.reads - before.reads)) +
                        text(", max difference %.6f", worst));
        }

        // Playing in real time (about twice as fast), the Disk lane's own threads serving it:
        // stopped first, so the chunk under the playhead is asked for ahead of play
        auto playWith = [&](std::shared_ptr<const audio::AudioData> data, bool sleep) {
            setClips(data);
            audio::Transport &transport = system.transport();
            transport.stop();
            transport.locate(0);
            std::vector<float> block(512), out;
            for (int i = 0; i < 8; ++i) {
                system.renderOffline(block.data(), 256, 48000.0);
                if (sleep)
                    std::this_thread::sleep_for(std::chrono::microseconds(2500));
            }
            transport.play();
            for (uint32_t done = 0; done < 6 * 48000; done += 256) {
                system.renderOffline(block.data(), 256, 48000.0);
                out.insert(out.end(), block.begin(), block.end());
                if (sleep)
                    std::this_thread::sleep_for(std::chrono::microseconds(2500));
            }
            transport.stop();
            setClips(nullptr);
            return out;
        };
        if (again) {
            again.reset(); // a fresh source: nothing of it in memory
            again = audio::loadAudioFile(wav, error, &memory);
        }
        if (again && again->streamed()) {
            const std::vector<float> reference = playWith(whole, false);
            memory.start(environment);
            const uint64_t before = memory.stats().underruns;
            const std::vector<float> played = playWith(again, true);
            const uint64_t underruns = memory.stats().underruns - before;
            memory.stop(environment);
            float loudest = 0.0f;
            const float worst = difference(reference, played, loudest);
            c.check(worst == 0.0f && underruns == 0, "Clips: the Disk lane keeps a mid-file start fed",
                    text("max difference %.6f", worst) + text(", %.0f underruns", static_cast<double>(underruns)));
        }

        // The browser's preview reads the same way
        if (again && again->streamed()) {
            auto &preview = system.add<audio::SoundPreview>();
            auto listen = [&](std::shared_ptr<const audio::AudioData> data) {
                preview.play(data);
                std::vector<float> block(512), out;
                for (int i = 0; i < 400; ++i) { // past the preload
                    system.renderOffline(block.data(), 256, 48000.0);
                    out.insert(out.end(), block.begin(), block.end());
                    memory.poll();
                }
                preview.stop();
                for (int i = 0; i < 4; ++i)
                    system.renderOffline(block.data(), 256, 48000.0);
                preview.collect();
                return out;
            };
            const std::vector<float> reference = listen(whole);
            const uint64_t before = memory.stats().underruns;
            const std::vector<float> heard = listen(again);
            float loudest = 0.0f;
            const float worst = difference(reference, heard, loudest);
            c.check(worst == 0.0f && memory.stats().underruns == before, "Clips: a streamed preview plays as from memory",
                    text("max difference %.6f", worst));
            system.remove(preview);
        }

        again.reset();
        streamed.reset();
        memory.poll();
        c.check(memory.stats().sources == 0 && memory.stats().preloadBytes == 0, "Clips: released clips let go of their sources",
                text("%.0f left", memory.stats().sources));
        std::vector<float> block(64);
        system.renderOffline(block.data(), 16, 48000.0);
        fs::remove_all(folder, ec);
    }

    // Loaded resources: what nothing else holds goes once idle long enough, or over the budget
    {
        env::ResourceCache<std::vector<char>> cache("Test", [](const std::vector<char> &v) { return static_cast<uint64_t>(v.size()); });
        for (const char *key : {"a", "b", "c"})
            cache.put(key, std::make_shared<std::vector<char>>(100));
        std::shared_ptr<std::vector<char>> held = cache.find("b");
        std::weak_ptr<std::vector<char>> a = cache.find("a");
        const double now = env::resourceClock();
        const uint64_t early = cache.release(now + 5.0, 10.0, 0);
        const env::ResourcePool::Usage before = cache.usage();
        const uint64_t late = cache.release(now + 20.0, 10.0, 0);
        c.check(early == 0 && before.items == 3 && before.idleItems == 2 && late == 200 && cache.size() == 1 &&
                    cache.contains("b") && a.expired(),
                "Resources: idle items go after the idle time, held ones stay");
        held.reset();
        // Released, b's idle time starts at the last sweep that saw it held
        const uint64_t soon = cache.release(now + 25.0, 10.0, 0);
        const uint64_t after = cache.release(now + 31.0, 10.0, 0);
        c.check(soon == 0 && after == 100 && cache.size() == 0, "Resources: idle time counts from the last use");

        env::Environment environment;
        env::ResourceConfig config;
        config.idleSeconds = 1e9;
        config.budgetBytes = 150;
        auto &manager = environment.add<env::ResourceManager>(config);
        env::ResourceCache<std::vector<char>> pool("Pool", [](const std::vector<char> &v) { return static_cast<uint64_t>(v.size()); });
        manager.add(pool);
        for (const char *key : {"old", "middle", "new"}) {
            pool.put(key, std::make_shared<std::vector<char>>(100));
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        const uint64_t overBudget = manager.sweep(env::resourceClock());
        c.check(overBudget == 200 && pool.contains("new") && !pool.contains("old") && !pool.contains("middle"),
                "Resources: over the budget the oldest idle go first");
        std::shared_ptr<std::vector<char>> keep = pool.find("new");
        const uint64_t heldTrim = manager.trim();
        keep.reset();
        const uint64_t trimmed = manager.trim();
        c.check(heldTrim == 0 && trimmed == 100 && manager.freedBytes() == 300 && manager.stats().size() == 1 &&
                    manager.stats()[0].freed == 300,
                "Resources: trim lets go of all that is unused, and counts it");
        manager.remove(pool);
    }

    std::printf("\n%s: %d failed\n", c.failures == 0 ? "All passed" : "FAILED", c.failures);
    return c.failures == 0 ? 0 : 1;
}

}
