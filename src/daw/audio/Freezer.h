#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "daw/audio/AudioPool.h"
#include "daw/audio/RenderJob.h"
#include "daw/library/Library.h"
#include "daw/model/Project.h"
#include "env/Scheduler.h"

namespace env {
class Environment;
}
namespace plugin {
class PluginManager;
}

namespace daw {

// What a render of a track is for (docs/design/freeze-plan.md)
enum class FreezeKind {
    Freeze,  // the track plays the file in place of its devices
    Bounce,  // a new audio track plays the file, the track muted
    Flatten, // the track becomes an audio track playing the file
};

// Renders tracks' sound (their instrument and effects, before the fader) into files, one at a time,
// for freezing, bouncing and flattening. A job copies the project as it is when it is started,
// keeping only the track and the tracks its effects are keyed by (going straight to the master, the
// master's effects gone), and renders it in an OfflineEngine at the device's rate: the track tapped
// before its fader, unmuted, from the song's start to its last clip's end and on until it is quiet
// (10 s at most). The render is written as a 32-bit float WAV into the folder given, its plugins'
// latency cut from the front so it lines up. Main thread, update() once a frame; what finishes is
// handed back by takeFinished() for the app to apply.
class Freezer {
public:
    struct Result {
        ObjectId track = kNoId;
        FreezeKind kind = FreezeKind::Freeze;
        std::string file;    // the file written (empty on failure or when cancelled)
        double seconds = 0.0;
        uint64_t hash = 0;   // freezeHash of the track as it was rendered
        std::string error;   // why it failed
        bool cancelled = false;
    };

    Freezer(env::Environment &env, plugin::PluginManager *plugins, InstrumentLibrary &library, AudioPool &pool);
    // Cancels and waits for what is still rendering
    ~Freezer();

    Freezer(const Freezer &) = delete;
    Freezer &operator=(const Freezer &) = delete;

    // Queues a render of track `index` of `project` (its sound as it is now) into `folder`, named after
    // it. False, with why, when the track can't be rendered (a bus, or one already queued).
    bool start(const Project &project, int index, FreezeKind kind, const std::string &folder, uint32_t sampleRate,
               std::string &why);
    void update();
    std::vector<Result> takeFinished();

    // The track's job: whether there is one, its progress (0-1; 0 while it waits), what it is doing,
    // and cancelling it
    bool busy(ObjectId track) const;
    float progress(ObjectId track) const;
    std::string status(ObjectId track) const;
    void cancel(ObjectId track);
    bool active() const { return !jobs_.empty(); }

private:
    struct Job;
    void startRender(Job &job);
    void finish(Job &job, Result result);

    env::Environment &env_;
    plugin::PluginManager *plugins_;
    InstrumentLibrary &library_;
    AudioPool &pool_;
    std::vector<std::unique_ptr<Job>> jobs_; // the first renders; the rest wait
    std::vector<Result> finished_;
};

}
