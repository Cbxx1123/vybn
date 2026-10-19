#pragma once

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "env/Daemon.h"
#include "env/Environment.h"
#include "env/MpmcQueue.h"

namespace env {
class Scheduler;
}

namespace audio {

class AudioSystem;
class MemoryManager;

// Per thread: while set on the calling thread, a chunk that isn't in memory is read there and then
// (as with MemoryManager::setOffline, but for this thread alone, so the live audio thread keeps
// playing silence). An offline engine sets it on its render thread and on its pool's workers.
void setRenderingOffline(bool on);
bool renderingOffline();

// A sample that is only partly in RAM: its first frames (the preload) stay in memory, so a
// note starts at once, and the rest is read from the sample cache on disk in chunks as voices
// get near them. The chunks live in the MemoryManager's pool and go again once nothing has
// played them for a while, so an instrument takes a fraction of the memory it would fully
// loaded. Made by the MemoryManager; immutable apart from which chunks are in memory.
//
// Instrument samples are 16-bit integers; audio clips (sampleBytes 4) are 32-bit floats, with
// half as many frames to a chunk so every chunk fits the pool's blocks.
//
// Real-time: chunk() and prefetch() are for the audio thread and its workers (several at once),
// and never block: a chunk that isn't in memory yet is asked for and comes back null.
class StreamSource {
public:
    uint32_t channels = 1;
    uint32_t frames = 0;
    double sampleRate = 44100.0;
    uint32_t sampleBytes = 2;   // 2: 16-bit integers; 4: 32-bit floats
    uint32_t preloadFrames = 0; // frames [0, preloadFrames) are in `preload` (or `preloadFloat`)
    uint32_t chunkFrames = 0;   // chunk k holds frames from preloadFrames + k * chunkFrames
    uint32_t chunkCount = 0;
    std::shared_ptr<const std::vector<int16_t>> preload; // interleaved 16-bit, like SampleZone::pcm
    std::shared_ptr<const std::vector<float>> preloadFloat; // a float source's, interleaved

    bool isFloat() const { return sampleBytes == 4; }
    // The chunk's frames (interleaved), or null while it isn't in memory, in which case it has
    // been asked for. Pointers stay valid until the end of the audio callback they were got in.
    const int16_t *chunk(uint32_t index) const;
    // The same for a float source
    const float *chunkFloat(uint32_t index) const { return reinterpret_cast<const float *>(chunk(index)); }
    // Asks for a chunk ahead of need, if it isn't in memory or on its way
    void prefetch(uint32_t index) const;
    // The chunk holding `frame` (at least preloadFrames)
    uint32_t chunkOf(uint32_t frame) const { return (frame - preloadFrames) / chunkFrames; }

    ~StreamSource();

private:
    friend class MemoryManager;
    StreamSource() = default;

    MemoryManager *manager_ = nullptr;
    uint32_t id_ = 0;
    std::string cachePath_;
    uint64_t dataOffset_ = 0; // where frame 0 starts in the cache file
    std::unique_ptr<std::atomic<int16_t *>[]> chunks_;
    std::unique_ptr<std::atomic<uint8_t>[]> state_; // 0 not in memory, 1 asked for, 2 in memory
    std::unique_ptr<std::atomic<uint32_t>[]> used_; // the audio callback that last read it
    mutable std::mutex fileMutex_;                   // disk threads
    std::FILE *file_ = nullptr;
    int busy_ = 0; // disk reads under way (under the manager's lock)
};

struct MemoryConfig {
    bool streaming = true;             // stream long samples from disk (else load them whole)
    uint32_t preloadFrames = 32768;    // kept in RAM per sample: about 0.7 s at 48 kHz
    uint32_t chunkFrames = 16384;      // read from disk at a time
    uint64_t budgetBytes = 128ull << 20; // the most the chunk pool holds
    double idleSeconds = 20.0;         // chunks unplayed this long are let go
    std::string cacheFolder;           // the sample cache: 16-bit copies of decoded samples
};

// The program's sample memory. Long, unlooped samples are kept as StreamSources: the first
// frames in RAM, the rest in a cache file of 16-bit audio (written once, the first time an
// instrument loads, and read straight back afterwards, so later loads only read the preloads).
//
// The disk I/O pooler is a poller on the Scheduler's Disk lane: audio threads put chunk requests
// on a lock-free queue and wake it; its threads read the chunks into blocks of a fixed-size pool
// (at most budgetBytes), evicting what was played longest ago when the pool is full and what
// hasn't been played for idleSeconds. Chunks read in the last few audio callbacks are never
// evicted, and a handshake on each chunk's use stamp (see evict) makes sure no voice is still
// reading a block that is reused.
//
// Add it after the Scheduler and the AudioSystem (its clock is the audio callback count).
// Loaders on background threads call openCached and create (openCachedFloat, createFloat and
// openFloatFile for audio clips). A StreamSource may outlive it (the AudioSystem's processors go
// after it): it then keeps only its preload, and plays silence past it.
class MemoryManager : public env::Daemon {
public:
    explicit MemoryManager(env::Environment &env, MemoryConfig config = {});
    ~MemoryManager() override;

    // Main thread. Budget and idle changes apply at once; preload, chunk size and streaming to
    // samples loaded afterwards.
    void configure(const MemoryConfig &config);
    MemoryConfig config() const;
    bool streaming() const { return streaming_.load(); }

    // Loader threads. A source from the cache for `sourcePath`, if the cache has an up-to-date
    // copy (same size and time as the file); else null.
    std::shared_ptr<StreamSource> openCached(const std::string &sourcePath);
    // Writes `pcm` (interleaved 16-bit) to the cache and returns a source for it, keeping only
    // the preload; null (with why in error) if it couldn't be written
    std::shared_ptr<StreamSource> create(const std::string &sourcePath, const std::vector<int16_t> &pcm,
                                         uint32_t channels, double sampleRate, std::string &error);
    // Audio clips: 32-bit float sources, so a recording streams back exactly as it was made.
    // A source from the cache's float copy of `sourcePath`, if up to date, with the `trailer`
    // stored beside it (a clip's overview); else null.
    std::shared_ptr<StreamSource> openCachedFloat(const std::string &sourcePath, std::vector<float> &trailer);
    // Writes the audio `read` gives (interleaved floats, up to `maxFrames` a call, 0 at the end)
    // to the cache, then `trailer`, which `read` may fill as it goes; keeps only the preload.
    // Null, with why in error, if it couldn't be written (or there was no audio).
    std::shared_ptr<StreamSource> createFloat(const std::string &sourcePath, uint32_t channels, double sampleRate,
                                              const std::function<size_t(float *out, size_t maxFrames)> &read,
                                              const std::vector<float> &trailer, std::string &error);
    // Streams floats straight from a file that already holds them (a recording's float WAV):
    // `frames` frames from `dataOffset`, the first of them given as `preload`. Null if it won't open.
    std::shared_ptr<StreamSource> openFloatFile(const std::string &path, uint64_t dataOffset, uint32_t channels,
                                                uint64_t frames, double sampleRate, std::vector<float> preload);
    // Whether a sample of this length is worth streaming at the current settings
    bool worthStreaming(uint64_t frames) const;
    uint32_t preloadFrames() const;

    struct Stats {
        uint64_t preloadBytes = 0; // the preloads of every source alive
        uint64_t poolBytes = 0;    // blocks allocated for chunks (in use or kept for reuse)
        uint64_t budgetBytes = 0;
        uint32_t sources = 0;
        uint32_t residentChunks = 0;
        uint64_t reads = 0;        // chunks read from disk
        uint64_t evictions = 0;
        uint64_t underruns = 0;    // times a voice reached a chunk that wasn't in memory yet
    };
    Stats stats() const;

    // Offline renders (exporting) run faster than the disk could stream ahead of them, so while
    // this is set a chunk that isn't in memory is read there and then, on the rendering thread,
    // instead of playing silence. Never while a device is running.
    void setOffline(bool offline) { offline_.store(offline); }
    bool offline() const { return offline_.load(std::memory_order_relaxed); }

    // Real-time (any audio thread)
    uint32_t epoch() const;
    void request(const StreamSource &source, uint32_t chunk);
    void noteUnderrun() { underruns_.fetch_add(1, std::memory_order_relaxed); }

    void start(env::Environment &env) override;
    void stop(env::Environment &env) override;

    // Serves waiting requests and evictions now, on the calling thread; true if it did anything.
    // The Disk lane does this; tests without one can call it.
    bool poll();

private:
    friend class StreamSource;
    struct Request {
        uint32_t source = 0;
        uint32_t chunk = 0;
    };
    struct Resident {
        uint32_t source;
        uint32_t chunk;
        uint32_t lastEpoch;  // the last `used` seen
        double lastPlayed;   // when it changed (steady clock seconds)
    };

    std::shared_ptr<StreamSource> make(const std::string &file, uint32_t channels, uint32_t frames, double sampleRate,
                                       uint64_t dataOffset, std::vector<int16_t> preload, std::vector<float> preloadFloat,
                                       uint32_t sampleBytes);
    std::shared_ptr<StreamSource> openCache(const std::string &sourcePath, bool floats, std::vector<float> *trailer);
    void release(StreamSource *source); // its last reference went
    void destroy(StreamSource *source);  // under lock_
    int16_t *takeBlock(uint32_t epochNow); // under lock_: a free block, or null
    bool evict(size_t resident, uint32_t epochNow); // under lock_; false if a reader just took it
    bool evictOldest(uint32_t epochNow);            // under lock_: the one played longest ago
    bool serve(const Request &request);
    // Offline: reads the chunk now (or waits for the disk thread already reading it)
    const int16_t *fetchNow(const StreamSource &source, uint32_t chunk);
    void housekeep();
    std::string cachePathFor(const std::string &sourcePath, bool floats) const;
    size_t blockBytes() const;
    // Chunks read this many audio callbacks back or fewer are never evicted: a reader may still
    // hold them. Readers keep a chunk's pointer only for the block they found it in, and offline
    // renders run within one call (the calling thread and the real-time pool), so then only the
    // current callback's chunks are in use, and a render in big blocks still finds room.
    static constexpr uint32_t kRecentEpochs = 8;
    uint32_t recentEpochs() const { return offline() || renderingOffline() ? 1 : kRecentEpochs; }

    // What a source's deleter reaches the manager through, so a source outliving it is safe
    struct Lifeline {
        std::mutex mutex;
        MemoryManager *manager = nullptr;
    };
    std::shared_ptr<Lifeline> life_;

    env::Environment &env_;
    env::Scheduler *scheduler_ = nullptr;
    AudioSystem *audio_ = nullptr;
    int poller_ = 0;

    mutable std::mutex lock_; // everything below that isn't atomic
    MemoryConfig config_;
    std::atomic<bool> streaming_{true};
    uint32_t nextId_ = 1;
    std::unordered_map<uint32_t, StreamSource *> sources_;
    std::vector<StreamSource *> dead_; // released, waiting for their reads to finish
    std::vector<Resident> resident_;
    std::vector<int16_t *> free_;
    uint64_t blocks_ = 0; // allocated
    uint32_t blockChunkFrames_ = 0; // the chunk size the pool's blocks were made for
    uint64_t preloadBytes_ = 0;
    double lastHousekeep_ = 0.0;
    bool polling_ = false; // the Disk lane is serving

    env::MpmcQueue<Request, 1024> requests_;
    std::atomic<uint64_t> reads_{0};
    std::atomic<uint64_t> evictions_{0};
    std::atomic<uint64_t> underruns_{0};
    std::atomic<uint32_t> clock_{0}; // the epoch when there's no audio system
    std::atomic<bool> offline_{false};
};

}
