#include "Memory.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <system_error>
#include <thread>

#include "AudioSystem.h"
#include "env/Scheduler.h"

namespace audio {

namespace fs = std::filesystem;

namespace {

thread_local bool renderingOfflineHere = false;

}

void setRenderingOffline(bool on)
{
    renderingOfflineHere = on;
}

bool renderingOffline()
{
    return renderingOfflineHere;
}

namespace {

constexpr char kMagic[8] = {'V', 'I', 'B', 'S', 'T', 'R', 'M', '1'};
// Version 1 files hold 16-bit samples; version 2, 32-bit floats (audio clips) with the header
// followed by the trailer's length, and the trailer after the audio
constexpr uint32_t kVersion = 1;
constexpr uint32_t kVersionFloat = 2;
constexpr size_t kKeepFree = 16;      // free blocks kept for reuse; more go back to the system

double now()
{
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

// The cache file's header, before the path and the audio
struct Header {
    char magic[8];
    uint32_t version;
    uint32_t channels;
    double sampleRate;
    uint32_t frames;
    uint32_t pathBytes;
    uint64_t sourceSize;
    int64_t sourceTime;
};

bool sourceStamp(const std::string &path, uint64_t &size, int64_t &time)
{
    std::error_code ec;
    const fs::path p = fs::u8path(path);
    size = fs::file_size(p, ec);
    if (ec)
        return false;
    time = static_cast<int64_t>(fs::last_write_time(p, ec).time_since_epoch().count());
    return !ec;
}

uint64_t preloadBytesOf(const StreamSource &s)
{
    return s.preload->size() * sizeof(int16_t) + s.preloadFloat->size() * sizeof(float);
}

uint64_t align64(uint64_t n)
{
    return (n + 63) & ~uint64_t(63);
}

std::FILE *openFile(const std::string &path, const char *mode)
{
#ifdef _WIN32
    const std::wstring wide = fs::u8path(path).wstring();
    const std::wstring wmode(mode, mode + std::strlen(mode));
    return _wfopen(wide.c_str(), wmode.c_str());
#else
    return std::fopen(path.c_str(), mode);
#endif
}

int seek(std::FILE *file, uint64_t offset)
{
#ifdef _WIN32
    return _fseeki64(file, static_cast<__int64>(offset), SEEK_SET);
#else
    return fseeko(file, static_cast<off_t>(offset), SEEK_SET);
#endif
}

}

// StreamSource

StreamSource::~StreamSource()
{
    if (file_)
        std::fclose(file_);
}

const int16_t *StreamSource::chunk(uint32_t index) const
{
    if (index >= chunkCount || !manager_)
        return nullptr;
    // Stamped before the pointer is taken: see MemoryManager::evict
    used_[index].store(manager_->epoch());
    if (const int16_t *data = chunks_[index].load())
        return data;
    if (manager_->offline() || renderingOffline()) {
        if (const int16_t *read = manager_->fetchNow(*this, index))
            return read;
    }
    manager_->noteUnderrun();
    manager_->request(*this, index);
    return nullptr;
}

void StreamSource::prefetch(uint32_t index) const
{
    if (index >= chunkCount || !manager_)
        return;
    if (state_[index].load(std::memory_order_relaxed) == 0)
        manager_->request(*this, index);
    else
        used_[index].store(manager_->epoch(), std::memory_order_relaxed); // wanted soon: keep it
}

// MemoryManager

MemoryManager::MemoryManager(env::Environment &env, MemoryConfig config)
    : env::Daemon("Memory"), life_(std::make_shared<Lifeline>()), env_(env)
{
    life_->manager = this;
    scheduler_ = env.find<env::Scheduler>();
    audio_ = env.find<AudioSystem>();
    if (config.cacheFolder.empty())
        config.cacheFolder = (fs::temp_directory_path() / "Vibin sample cache").u8string();
    config.chunkFrames = std::max<uint32_t>(config.chunkFrames, 1024);
    config_ = config;
    streaming_ = config.streaming;
    blockChunkFrames_ = config.chunkFrames;
}

MemoryManager::~MemoryManager()
{
    if (poller_ && scheduler_)
        scheduler_->removePoller(poller_);
    {
        // Sources released from now on (the AudioSystem's processors go after this daemon) only
        // delete themselves
        std::lock_guard<std::mutex> life(life_->mutex);
        life_->manager = nullptr;
    }
    std::lock_guard<std::mutex> lock(lock_);
    for (StreamSource *s : dead_)
        destroy(s);
    dead_.clear();
    // Sources still alive (they shouldn't be) keep only their preloads; the pool goes
    for (auto &entry : sources_) {
        StreamSource *source = entry.second;
        for (uint32_t k = 0; k < source->chunkCount; ++k)
            delete[] source->chunks_[k].exchange(nullptr);
        source->manager_ = nullptr;
    }
    for (int16_t *b : free_)
        delete[] b;
}

void MemoryManager::start(env::Environment &)
{
    if (scheduler_) {
        polling_ = true;
        poller_ = scheduler_->addPoller(env::Lane::Disk, [this] { return poll(); }, 250);
    }
}

void MemoryManager::stop(env::Environment &)
{
    if (poller_ && scheduler_)
        scheduler_->removePoller(poller_);
    poller_ = 0;
    std::lock_guard<std::mutex> lock(lock_);
    polling_ = false;
}

void MemoryManager::configure(const MemoryConfig &config)
{
    std::lock_guard<std::mutex> lock(lock_);
    const uint32_t chunk = config_.chunkFrames; // the pool's blocks are this size
    config_ = config;
    config_.chunkFrames = chunk;
    if (config_.cacheFolder.empty())
        config_.cacheFolder = (fs::temp_directory_path() / "Vibin sample cache").u8string();
    streaming_ = config.streaming;
}

MemoryConfig MemoryManager::config() const
{
    std::lock_guard<std::mutex> lock(lock_);
    return config_;
}

bool MemoryManager::worthStreaming(uint64_t frames) const
{
    std::lock_guard<std::mutex> lock(lock_);
    // At least two chunks past the preload, or streaming saves too little to bother; and no
    // longer than a source can count (a day at 48 kHz)
    return config_.streaming && frames > uint64_t(config_.preloadFrames) + 2 * config_.chunkFrames &&
           frames <= UINT32_MAX;
}

uint32_t MemoryManager::preloadFrames() const
{
    std::lock_guard<std::mutex> lock(lock_);
    return config_.preloadFrames;
}

size_t MemoryManager::blockBytes() const
{
    return static_cast<size_t>(blockChunkFrames_) * 2 * sizeof(int16_t); // room for stereo
}

uint32_t MemoryManager::epoch() const
{
    return audio_ ? static_cast<uint32_t>(audio_->callbacks()) : clock_.load(std::memory_order_relaxed);
}

void MemoryManager::request(const StreamSource &source, uint32_t chunk)
{
    uint8_t expected = 0;
    if (!source.state_[chunk].compare_exchange_strong(expected, 1, std::memory_order_acq_rel))
        return; // on its way, or in memory
    source.used_[chunk].store(epoch(), std::memory_order_relaxed);
    if (!requests_.push({source.id_, chunk})) {
        source.state_[chunk].store(0, std::memory_order_relaxed); // full: asked again next time
        return;
    }
    if (scheduler_)
        scheduler_->wake(env::Lane::Disk);
}

std::string MemoryManager::cachePathFor(const std::string &sourcePath, bool floats) const
{
    // FNV-1a of the path names the file; the header holds the path to make sure
    uint64_t h = 1469598103934665603ull;
    for (unsigned char c : sourcePath) {
        h ^= c;
        h *= 1099511628211ull;
    }
    // A file a clip plays and an instrument uses keeps a copy of each kind
    char name[32];
    std::snprintf(name, sizeof(name), floats ? "%016llx-f.vbs" : "%016llx.vbs", static_cast<unsigned long long>(h));
    return (fs::u8path(config_.cacheFolder) / name).u8string();
}

std::shared_ptr<StreamSource> MemoryManager::make(const std::string &path, uint32_t channels, uint32_t frames,
                                                  double sampleRate, uint64_t dataOffset, std::vector<int16_t> preload,
                                                  std::vector<float> preloadFloat, uint32_t sampleBytes)
{
    std::FILE *file = openFile(path, "rb");
    if (!file)
        return nullptr;
    StreamSource *s = new StreamSource();
    s->manager_ = this;
    s->channels = channels;
    s->frames = frames;
    s->sampleRate = sampleRate;
    s->sampleBytes = sampleBytes;
    s->preloadFrames = static_cast<uint32_t>((sampleBytes == 4 ? preloadFloat.size() : preload.size()) / channels);
    // A block holds a stereo chunk of 16-bit samples, or half as many frames of floats
    s->chunkFrames = sampleBytes == 4 ? blockChunkFrames_ / 2 : blockChunkFrames_;
    s->chunkCount = frames > s->preloadFrames ? (frames - s->preloadFrames + s->chunkFrames - 1) / s->chunkFrames : 0;
    s->preload = std::make_shared<const std::vector<int16_t>>(std::move(preload));
    s->preloadFloat = std::make_shared<const std::vector<float>>(std::move(preloadFloat));
    s->cachePath_ = path;
    s->dataOffset_ = dataOffset;
    s->file_ = file;
    s->chunks_ = std::make_unique<std::atomic<int16_t *>[]>(s->chunkCount);
    s->state_ = std::make_unique<std::atomic<uint8_t>[]>(s->chunkCount);
    s->used_ = std::make_unique<std::atomic<uint32_t>[]>(s->chunkCount);
    for (uint32_t k = 0; k < s->chunkCount; ++k) {
        s->chunks_[k].store(nullptr, std::memory_order_relaxed);
        s->state_[k].store(0, std::memory_order_relaxed);
        s->used_[k].store(0, std::memory_order_relaxed);
    }
    {
        std::lock_guard<std::mutex> lock(lock_);
        s->id_ = nextId_++;
        sources_[s->id_] = s;
        preloadBytes_ += preloadBytesOf(*s);
    }
    return std::shared_ptr<StreamSource>(s, [life = life_](StreamSource *dying) {
        std::lock_guard<std::mutex> lock(life->mutex);
        if (life->manager)
            life->manager->release(dying);
        else
            delete dying; // the manager went first
    });
}

std::shared_ptr<StreamSource> MemoryManager::openCached(const std::string &sourcePath)
{
    return openCache(sourcePath, false, nullptr);
}

std::shared_ptr<StreamSource> MemoryManager::openCachedFloat(const std::string &sourcePath, std::vector<float> &trailer)
{
    return openCache(sourcePath, true, &trailer);
}

std::shared_ptr<StreamSource> MemoryManager::openCache(const std::string &sourcePath, bool floats,
                                                       std::vector<float> *trailer)
{
    uint64_t size = 0;
    int64_t time = 0;
    if (!sourceStamp(sourcePath, size, time))
        return nullptr;
    uint32_t preloadFrames;
    std::string path;
    {
        std::lock_guard<std::mutex> lock(lock_);
        if (!config_.streaming)
            return nullptr;
        preloadFrames = config_.preloadFrames;
        path = cachePathFor(sourcePath, floats);
    }
    std::FILE *file = openFile(path, "rb");
    if (!file)
        return nullptr;
    Header h{};
    uint64_t trailerValues = 0;
    std::string stored;
    bool ok = std::fread(&h, sizeof(h), 1, file) == 1 && std::memcmp(h.magic, kMagic, 8) == 0 &&
              h.version == (floats ? kVersionFloat : kVersion) && (h.channels == 1 || h.channels == 2) && h.frames > 0 &&
              h.pathBytes < 4096 && h.sourceSize == size && h.sourceTime == time;
    if (ok && floats)
        ok = std::fread(&trailerValues, sizeof(trailerValues), 1, file) == 1 && trailerValues < (1ull << 32);
    if (ok) {
        stored.resize(h.pathBytes);
        ok = std::fread(&stored[0], 1, h.pathBytes, file) == h.pathBytes && stored == sourcePath;
    }
    const uint32_t sampleBytes = floats ? 4 : 2;
    const uint64_t offset = align64(sizeof(Header) + (floats ? sizeof(uint64_t) : 0) + h.pathBytes);
    std::vector<int16_t> preload;
    std::vector<float> preloadFloat;
    if (ok) {
        const size_t values = static_cast<size_t>(std::min(preloadFrames, h.frames)) * h.channels;
        ok = seek(file, offset) == 0;
        if (ok && floats) {
            preloadFloat.resize(values);
            ok = std::fread(preloadFloat.data(), sizeof(float), values, file) == values;
        } else if (ok) {
            preload.resize(values);
            ok = std::fread(preload.data(), sizeof(int16_t), values, file) == values;
        }
    }
    if (ok && trailer) {
        trailer->resize(static_cast<size_t>(trailerValues));
        ok = seek(file, offset + static_cast<uint64_t>(h.frames) * h.channels * sampleBytes) == 0 &&
             std::fread(trailer->data(), sizeof(float), trailer->size(), file) == trailer->size();
    }
    std::fclose(file);
    if (!ok)
        return nullptr;
    return make(path, h.channels, h.frames, h.sampleRate, offset, std::move(preload), std::move(preloadFloat), sampleBytes);
}

std::shared_ptr<StreamSource> MemoryManager::create(const std::string &sourcePath, const std::vector<int16_t> &pcm,
                                                    uint32_t channels, double sampleRate, std::string &error)
{
    uint64_t size = 0;
    int64_t time = 0;
    if (channels < 1 || channels > 2 || pcm.empty() || !sourceStamp(sourcePath, size, time)) {
        error = "Can't stream " + sourcePath;
        return nullptr;
    }
    std::string path, folder;
    uint32_t preloadFrames;
    {
        std::lock_guard<std::mutex> lock(lock_);
        path = cachePathFor(sourcePath, false);
        folder = config_.cacheFolder;
        preloadFrames = config_.preloadFrames;
    }
    std::error_code ec;
    fs::create_directories(fs::u8path(folder), ec);
    // Written beside it, then renamed, so a half-written cache is never read
    const std::string temp = path + ".part";
    std::FILE *file = openFile(temp, "wb");
    if (!file) {
        error = "Can't write the sample cache in " + folder;
        return nullptr;
    }
    Header h{};
    std::memcpy(h.magic, kMagic, 8);
    h.version = kVersion;
    h.channels = channels;
    h.sampleRate = sampleRate;
    h.frames = static_cast<uint32_t>(pcm.size() / channels);
    h.pathBytes = static_cast<uint32_t>(sourcePath.size());
    h.sourceSize = size;
    h.sourceTime = time;
    const uint64_t offset = align64(sizeof(Header) + h.pathBytes);
    std::vector<char> pad(static_cast<size_t>(offset - sizeof(Header) - h.pathBytes), 0);
    bool ok = std::fwrite(&h, sizeof(h), 1, file) == 1 &&
              std::fwrite(sourcePath.data(), 1, sourcePath.size(), file) == sourcePath.size() &&
              (pad.empty() || std::fwrite(pad.data(), 1, pad.size(), file) == pad.size()) &&
              std::fwrite(pcm.data(), sizeof(int16_t), pcm.size(), file) == pcm.size();
    ok = std::fclose(file) == 0 && ok;
    if (ok) {
        fs::rename(fs::u8path(temp), fs::u8path(path), ec);
        ok = !ec;
    }
    if (!ok) {
        fs::remove(fs::u8path(temp), ec);
        error = "Can't write the sample cache in " + folder;
        return nullptr;
    }
    const size_t keep = std::min<size_t>(pcm.size(), static_cast<size_t>(preloadFrames) * channels);
    return make(path, channels, h.frames, sampleRate, offset, std::vector<int16_t>(pcm.begin(), pcm.begin() + keep), {}, 2);
}

std::shared_ptr<StreamSource> MemoryManager::createFloat(const std::string &sourcePath, uint32_t channels, double sampleRate,
                                                         const std::function<size_t(float *, size_t)> &read,
                                                         const std::vector<float> &trailer, std::string &error)
{
    uint64_t size = 0;
    int64_t time = 0;
    if (channels < 1 || channels > 2 || !sourceStamp(sourcePath, size, time)) {
        error = "Can't stream " + sourcePath;
        return nullptr;
    }
    std::string path, folder;
    uint32_t preloadFrames;
    {
        std::lock_guard<std::mutex> lock(lock_);
        path = cachePathFor(sourcePath, true);
        folder = config_.cacheFolder;
        preloadFrames = config_.preloadFrames;
    }
    std::error_code ec;
    fs::create_directories(fs::u8path(folder), ec);
    const std::string temp = path + ".part";
    std::FILE *file = openFile(temp, "wb");
    if (!file) {
        error = "Can't write the sample cache in " + folder;
        return nullptr;
    }
    // The header's lengths are written again once the audio is in
    Header h{};
    std::memcpy(h.magic, kMagic, 8);
    h.version = kVersionFloat;
    h.channels = channels;
    h.sampleRate = sampleRate;
    h.pathBytes = static_cast<uint32_t>(sourcePath.size());
    h.sourceSize = size;
    h.sourceTime = time;
    uint64_t trailerValues = 0;
    const uint64_t offset = align64(sizeof(Header) + sizeof(uint64_t) + h.pathBytes);
    std::vector<char> pad(static_cast<size_t>(offset - sizeof(Header) - sizeof(uint64_t) - h.pathBytes), 0);
    bool ok = std::fwrite(&h, sizeof(h), 1, file) == 1 && std::fwrite(&trailerValues, sizeof(trailerValues), 1, file) == 1 &&
              std::fwrite(sourcePath.data(), 1, sourcePath.size(), file) == sourcePath.size() &&
              (pad.empty() || std::fwrite(pad.data(), 1, pad.size(), file) == pad.size());
    // The audio a piece at a time, so a long file is never whole in memory
    constexpr size_t kPiece = 16384;
    std::vector<float> piece(kPiece * channels);
    std::vector<float> preload;
    const size_t preloadValues = static_cast<size_t>(preloadFrames) * channels;
    uint64_t frames = 0;
    while (ok) {
        const size_t n = std::min(read(piece.data(), kPiece), kPiece);
        if (n == 0)
            break;
        const size_t values = n * channels;
        ok = std::fwrite(piece.data(), sizeof(float), values, file) == values;
        if (preload.size() < preloadValues)
            preload.insert(preload.end(), piece.begin(),
                           piece.begin() + static_cast<std::ptrdiff_t>(std::min(values, preloadValues - preload.size())));
        frames += n;
        ok = ok && frames <= UINT32_MAX;
    }
    h.frames = static_cast<uint32_t>(frames);
    trailerValues = trailer.size();
    ok = ok && frames > 0 &&
         (trailer.empty() || std::fwrite(trailer.data(), sizeof(float), trailer.size(), file) == trailer.size()) &&
         seek(file, 0) == 0 && std::fwrite(&h, sizeof(h), 1, file) == 1 &&
         std::fwrite(&trailerValues, sizeof(trailerValues), 1, file) == 1;
    ok = std::fclose(file) == 0 && ok;
    if (ok) {
        fs::rename(fs::u8path(temp), fs::u8path(path), ec);
        ok = !ec;
    }
    if (!ok) {
        fs::remove(fs::u8path(temp), ec);
        error = frames == 0 ? "No audio in " + sourcePath : "Can't write the sample cache in " + folder;
        return nullptr;
    }
    return make(path, channels, h.frames, sampleRate, offset, {}, std::move(preload), 4);
}

std::shared_ptr<StreamSource> MemoryManager::openFloatFile(const std::string &path, uint64_t dataOffset, uint32_t channels,
                                                           uint64_t frames, double sampleRate, std::vector<float> preload)
{
    if (channels < 1 || channels > 2 || frames == 0 || frames > UINT32_MAX || preload.size() > frames * channels)
        return nullptr;
    return make(path, channels, static_cast<uint32_t>(frames), sampleRate, dataOffset, {}, std::move(preload), 4);
}

void MemoryManager::release(StreamSource *source)
{
    std::unique_lock<std::mutex> lock(lock_);
    if (source->manager_ == nullptr) { // the manager went first
        lock.unlock();
        delete source;
        return;
    }
    preloadBytes_ -= preloadBytesOf(*source);
    if (polling_) {
        dead_.push_back(source); // the disk threads may still be reading it
        lock.unlock();
        if (scheduler_)
            scheduler_->wake(env::Lane::Disk);
        return;
    }
    destroy(source);
}

void MemoryManager::destroy(StreamSource *source)
{
    // Its chunks' blocks go straight back: nothing plays a source nobody holds
    for (size_t i = 0; i < resident_.size();) {
        if (resident_[i].source == source->id_) {
            int16_t *block = source->chunks_[resident_[i].chunk].exchange(nullptr);
            if (block)
                free_.push_back(block);
            resident_[i] = resident_.back();
            resident_.pop_back();
        } else {
            ++i;
        }
    }
    sources_.erase(source->id_);
    delete source;
}

int16_t *MemoryManager::takeBlock(uint32_t epochNow)
{
    if (free_.empty()) {
        const uint64_t maxBlocks = std::max<uint64_t>(4, config_.budgetBytes / blockBytes());
        if (blocks_ < maxBlocks) {
            ++blocks_;
            return new int16_t[blockBytes() / sizeof(int16_t)];
        }
        // Full: let go of the chunk played longest ago, unless it is playing now
        if (!evictOldest(epochNow))
            return nullptr;
    }
    int16_t *b = free_.back();
    free_.pop_back();
    return b;
}

bool MemoryManager::evictOldest(uint32_t epochNow)
{
    // Oldest first; one a reader takes meanwhile is kept, and the next oldest tried
    for (int tries = 0; tries < 4; ++tries) {
        size_t oldest = resident_.size();
        uint32_t oldestUse = 0;
        for (size_t i = 0; i < resident_.size(); ++i) {
            const Resident &r = resident_[i];
            auto it = sources_.find(r.source);
            if (it == sources_.end())
                continue;
            const uint32_t used = it->second->used_[r.chunk].load(std::memory_order_relaxed);
            if (epochNow - used < recentEpochs())
                continue;
            if (oldest == resident_.size() || static_cast<int32_t>(used - oldestUse) < 0) {
                oldest = i;
                oldestUse = used;
            }
        }
        if (oldest == resident_.size())
            return false;
        if (evict(oldest, epochNow))
            return true;
    }
    return false;
}

bool MemoryManager::evict(size_t index, uint32_t epochNow)
{
    const Resident r = resident_[index];
    auto it = sources_.find(r.source);
    if (it == sources_.end()) {
        resident_[index] = resident_.back();
        resident_.pop_back();
        return false;
    }
    StreamSource &s = *it->second;
    // Readers stamp a chunk as used before they take its pointer, and this takes the pointer
    // before looking at the stamp (all sequentially consistent), so either a reader finds it
    // gone, or this sees the reader's stamp and puts it back. A block taken here has no reader,
    // so it can be used again at once.
    int16_t *block = s.chunks_[r.chunk].exchange(nullptr);
    if (epochNow - s.used_[r.chunk].load() < recentEpochs()) {
        s.chunks_[r.chunk].store(block);
        return false;
    }
    resident_[index] = resident_.back();
    resident_.pop_back();
    s.state_[r.chunk].store(0);
    if (block)
        free_.push_back(block);
    evictions_.fetch_add(1, std::memory_order_relaxed);
    return true;
}

bool MemoryManager::serve(const Request &request)
{
    StreamSource *s = nullptr;
    int16_t *block = nullptr;
    {
        std::lock_guard<std::mutex> lock(lock_);
        auto it = sources_.find(request.source);
        if (it == sources_.end() || std::find(dead_.begin(), dead_.end(), it->second) != dead_.end())
            return false;
        s = it->second;
        if (request.chunk >= s->chunkCount || s->state_[request.chunk].load() != 1)
            return false;
        block = takeBlock(epoch());
        if (!block) {
            s->state_[request.chunk].store(0); // no room now; the voice asks again
            return false;
        }
        ++s->busy_;
    }
    // The read, outside the manager's lock: other disk threads read other chunks meanwhile
    const uint32_t first = s->preloadFrames + request.chunk * s->chunkFrames;
    const uint32_t frames = std::min(s->chunkFrames, s->frames - first);
    const size_t values = static_cast<size_t>(frames) * s->channels;
    bool ok;
    {
        std::lock_guard<std::mutex> file(s->fileMutex_);
        ok = seek(s->file_, s->dataOffset_ + static_cast<uint64_t>(first) * s->channels * s->sampleBytes) == 0 &&
             std::fread(block, s->sampleBytes, values, s->file_) == values;
    }
    std::lock_guard<std::mutex> lock(lock_);
    --s->busy_;
    if (!ok) {
        std::memset(block, 0, values * s->sampleBytes); // a damaged cache plays silence, not noise
    }
    s->chunks_[request.chunk].store(block, std::memory_order_release);
    s->state_[request.chunk].store(2, std::memory_order_release);
    resident_.push_back({request.source, request.chunk, s->used_[request.chunk].load(), now()});
    reads_.fetch_add(1, std::memory_order_relaxed);
    return true;
}

const int16_t *MemoryManager::fetchNow(const StreamSource &source, uint32_t chunk)
{
    for (int attempt = 0; attempt < 1000; ++attempt) {
        source.used_[chunk].store(epoch());
        if (const int16_t *data = source.chunks_[chunk].load())
            return data;
        uint8_t expected = 0;
        if (source.state_[chunk].compare_exchange_strong(expected, 1)) {
            source.used_[chunk].store(epoch(), std::memory_order_relaxed);
            serve({source.id_, chunk}); // on this thread
            continue;
        }
        // Asked for already: served by whoever pops it (a disk thread, or this one)
        Request request;
        if (requests_.pop(request))
            serve(request);
        else
            std::this_thread::yield();
    }
    return nullptr;
}

void MemoryManager::housekeep()
{
    std::lock_guard<std::mutex> lock(lock_);
    const double t = now();
    // Released sources, once no read of theirs is under way (any request still queued for one
    // finds it gone)
    for (size_t i = 0; i < dead_.size();) {
        if (dead_[i]->busy_ == 0) {
            destroy(dead_[i]);
            dead_[i] = dead_.back();
            dead_.pop_back();
        } else {
            ++i;
        }
    }
    const uint32_t epochNow = epoch();
    if (t - lastHousekeep_ < 0.25)
        return;
    lastHousekeep_ = t;
    // Chunks nobody has played for a while
    for (size_t i = 0; i < resident_.size();) {
        Resident &r = resident_[i];
        auto it = sources_.find(r.source);
        const uint32_t used = it == sources_.end() ? r.lastEpoch : it->second->used_[r.chunk].load();
        if (used != r.lastEpoch) {
            r.lastEpoch = used;
            r.lastPlayed = t;
        }
        if (t - r.lastPlayed > config_.idleSeconds && epochNow - used >= kRecentEpochs && evict(i, epochNow))
            continue; // the last one moved into its place
        ++i;
    }
    // Spare blocks beyond a few go back to the system, so memory falls when playing stops
    while (free_.size() > kKeepFree) {
        delete[] free_.back();
        free_.pop_back();
        --blocks_;
    }
}

bool MemoryManager::poll()
{
    housekeep(); // blocks come free before requests want them
    bool did = false;
    Request request;
    while (requests_.pop(request))
        did = serve(request) || did;
    return did;
}

MemoryManager::Stats MemoryManager::stats() const
{
    std::lock_guard<std::mutex> lock(lock_);
    Stats s;
    s.preloadBytes = preloadBytes_;
    s.poolBytes = blocks_ * blockBytes();
    s.budgetBytes = config_.budgetBytes;
    s.sources = static_cast<uint32_t>(sources_.size() - dead_.size());
    s.residentChunks = static_cast<uint32_t>(resident_.size());
    s.reads = reads_.load();
    s.evictions = evictions_.load();
    s.underruns = underruns_.load();
    return s;
}

}
