#pragma once

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "daw/model/Project.h"
#include "audio/AudioFile.h"
#include "audio/Memory.h"
#include "audio/Warp.h"
#include "env/Environment.h"
#include "env/Resources.h"
#include "env/Scheduler.h"

namespace daw {

// A clip's stretched sound (docs/design/warping-plan.md): the whole file laid at the clip's place,
// its seconds the timeline's
struct WarpedSound {
    std::shared_ptr<const audio::AudioData> data; // null while none has been made for the clip
    audio::ClipWarp warp; // what it was made with: the clip's, or an earlier one while the new one is made
    bool exact = false;   // made from the clip's warp and the tempo as they are now
};

// What a sound file's hits are: its transients (seconds) and the tempo they suggest (0 for none)
struct FileOnsets {
    std::vector<double> transients;
    double tempo = 0.0;
};

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
    // Lets go of files no clip uses any more (and their stretched sounds)
    void keepOnly(const std::set<std::string> &paths);

    // The stretched sound of a warped clip on `timing`, made on a Background job when there isn't
    // one yet (its file must be loaded: get). While it is being made the clip gets the last one made
    // for it (by id), or nothing; a newer request for the same clip cancels the one being made, so a
    // marker being dragged renders once it stops. version() goes up as one arrives.
    WarpedSound warped(const Clip &clip, const audio::TempoMap &timing);
    // Whether the clip's exact stretched sound is ready (asking for it if not)
    bool warpReady(const Clip &clip, const audio::TempoMap &timing) { return warped(clip, timing).exact; }
    // A file's transients and tempo, found once on a Background job; null until they are (or while
    // the file isn't loaded). version() goes up when they arrive.
    const FileOnsets *onsets(const std::string &path);

private:
    env::Environment &env_;
    env::ResourceCache<const audio::AudioData> files_{"Sound files", [](const audio::AudioData &d) { return static_cast<uint64_t>(d.bytes()); }};
    env::ResourceManager *resources_ = nullptr;
    audio::MemoryManager *memory_ = nullptr;
    std::map<std::string, std::string> errors_;
    std::set<std::string> loading_;

    // Stretched sounds by key (the file, '#', and warpKey), what each was made with, and per clip (by
    // id) the last made for it, the one being made (with its cancel flag) and the newest asked for since
    struct WarpJob {
        std::string key, path;
        audio::ClipWarp warp;
        audio::TempoMap timing;
        double anchor = 0.0;
    };
    struct WarpRunning {
        std::string key;
        std::shared_ptr<std::atomic<bool>> cancel;
    };
    void startWarp(ObjectId clip, WarpJob job);
    env::ResourceCache<const audio::AudioData> warps_{"Warped sounds", [](const audio::AudioData &d) { return static_cast<uint64_t>(d.bytes()); }};
    std::map<std::string, audio::ClipWarp> warpOf_;
    std::set<std::string> warping_;
    std::map<ObjectId, std::string> lastWarp_;
    std::map<ObjectId, WarpRunning> runningWarp_;
    std::map<ObjectId, WarpJob> queuedWarp_;
    std::map<std::string, FileOnsets> onsets_;
    std::set<std::string> findingOnsets_;
    std::shared_ptr<std::atomic<bool>> closing_ = std::make_shared<std::atomic<bool>>(false);

    env::JobGroup jobs_; // its background jobs, waited for when it goes
    uint64_t version_ = 0;
};

}
