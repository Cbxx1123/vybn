#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "audio/AudioAnalysis.h"
#include "audio/AudioFile.h"
#include "audio/Memory.h"
#include "env/Environment.h"
#include "env/Resources.h"
#include "env/Scheduler.h"

namespace daw {

// The sound files the project's audio clips play, loaded once each and shared. Long ones stream
// through the environment's MemoryManager (when it streams): only their overview and preload stay
// in RAM, and the rest is read from the sample cache (or a recording's own WAV) as it plays.
// get() returns a file that is ready, or starts loading it on a background thread and returns
// null until it is (the result arrives on the main thread through Environment::post, and
// version() goes up). Recordings made in memory are added directly. A file stays while a clip
// player holds it (or something asks for it); once nothing has for a while the ResourceManager
// lets it go, and it loads again from disk when next asked for. Main thread only.
class AudioPool {
public:
    explicit AudioPool(env::Environment &env);
    ~AudioPool(); // joins the loaders

    AudioPool(const AudioPool &) = delete;
    AudioPool &operator=(const AudioPool &) = delete;

    std::shared_ptr<const audio::AudioData> get(const std::string &path);
    bool loading(const std::string &path) const { return loading_.count(path) > 0; }
    // Why a file couldn't be loaded (empty if it could, or hasn't been tried)
    std::string error(const std::string &path) const;
    void add(const std::string &path, std::shared_ptr<const audio::AudioData> data);
    // Loads a file now, on the calling thread (streamed if long), and adds it; null with why
    std::shared_ptr<const audio::AudioData> loadNow(const std::string &path, std::string &error);
    // Adds a recording just written as a 32-bit float WAV at `path` from these samples: a long
    // one streams from that file, and its samples are let go
    void addRecording(const std::string &path, double sampleRate, uint32_t channels, std::vector<float> samples);
    // Goes up whenever a file finishes loading (or fails), so users of get() can try again
    uint64_t version() const { return version_; }
    // Lets go of files no clip uses any more
    void keepOnly(const std::set<std::string> &paths);

    // A loaded file's analysis (its transients and tempo, for warping): ready, or started on a
    // background thread once the file has loaded and null until it is (its arrival moves version()).
    // Kept while the file is.
    std::shared_ptr<const audio::AudioAnalysis> analysis(const std::string &path);
    // The same now, on the calling thread (a file just imported), loading the file first if need be
    std::shared_ptr<const audio::AudioAnalysis> analyseNow(const std::string &path);

private:
    env::Environment &env_;
    env::ResourceCache<const audio::AudioData> files_{"Sound files", [](const audio::AudioData &d) { return static_cast<uint64_t>(d.bytes()); }};
    env::ResourceManager *resources_ = nullptr;
    audio::MemoryManager *memory_ = nullptr;
    std::map<std::string, std::string> errors_;
    std::set<std::string> loading_;
    std::map<std::string, std::shared_ptr<const audio::AudioAnalysis>> analyses_;
    std::set<std::string> analysing_;
    env::JobGroup jobs_; // its background jobs, waited for when it goes
    uint64_t version_ = 0;
};

}
