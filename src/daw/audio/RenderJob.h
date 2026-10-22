#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "daw/audio/AudioBridge.h"
#include "daw/model/ExportModel.h"
#include "daw/model/ProjectStore.h"
#include "audio/AudioSystem.h"
#include "audio/Midi.h"
#include "env/RealtimePool.h"
#include "env/Scheduler.h"

namespace env {
class Environment;
}
namespace plugin {
class PluginManager;
}

namespace daw {

// Whether a project's sound is ready to render: every library instrument a sampler track plays has
// loaded and been handed to its sampler (or failed, or isn't installed, which plays silent as it
// does live), and every audio clip's file has loaded (or failed). `waiting` names what isn't yet.
// Reading the pool asks for the files it doesn't have, so it is for the main thread.
bool renderReady(const Project &project, AudioBridge &bridge, InstrumentLibrary &library, AudioPool &pool,
                 std::string &waiting);

// A project rendered offline, on its own: a deviceless AudioSystem with its own pool of render
// threads, a MidiHub, a private copy of the project (a ProjectStore with every track disarmed) and
// an AudioBridge that builds the graph from it, as DawApp does for the live app. The live engine
// keeps playing while this one renders; the two share only the sample pool and the library's
// loaded instruments, which both only read.
//
// Threads: prepare() runs on the main thread, once a frame, until it returns true. After that the
// render thread owns the engine (RenderPass runs there) until it is done with it; the main thread
// does not touch it meanwhile. The engine's plugins run offline (their instances wait for each
// block rather than passing the sound through), and the streamed samples it reads are fetched when
// the render asks for them (audio::setRenderingOffline on its own threads, not a global flag, so
// the live engine keeps its real-time behaviour).
class OfflineEngine {
public:
    // `masterGain` is the live master fader's value (linear), which isn't part of the project.
    // `pooled` false mixes the tracks on the render thread alone (the self-test checks both give the
    // same samples).
    OfflineEngine(env::Environment &env, plugin::PluginManager *plugins, InstrumentLibrary &library, AudioPool &pool,
                  Project snapshot, float masterGain, uint32_t sampleRate, bool pooled = true);
    ~OfflineEngine();

    OfflineEngine(const OfflineEngine &) = delete;
    OfflineEngine &operator=(const OfflineEngine &) = delete;

    // Main thread, once a frame: syncs the graph with the snapshot and returns true once every
    // instrument and sound file is in and no plugin is still settling. Names what it waits for.
    bool prepare(std::string &waiting);
    bool ready() const { return ready_; }

    // Render thread (or the owner of the engine), once prepare() has returned true. Sets the taps:
    // each track in `tracks` copies its block into its tap, before its fader (`preFader`) or after
    // it; `unmuted` renders every track as if it weren't muted (for stems of muted tracks). Then
    // settles for a quarter second, as the old exporter did, and plays from `startBeat`. Taps and
    // rate are set at the start of a render, so one engine can run several passes in turn.
    void beginRender(double startBeat, const std::vector<int> &tracks, bool preFader, bool unmuted);
    // Stops the transport and removes every tap
    void endRender();
    // Renders up to AudioSystem::kMaxBlock frames: interleaved stereo of the master output. A tap
    // holds this call's frames only, so read taps after each call (and call with fewer frames than
    // a block of the engine's, so the taps hold every frame).
    void render(float *out, uint32_t frames);
    // The taps of a track, valid after render() for the frames it rendered (null when untapped)
    const float *tapLeft(int track) const;
    const float *tapRight(int track) const;

    // The rate the engine renders at. Main thread only, and never during a render: setRate prepares
    // the graph at the new rate and marks the engine not ready, so the next prepare() waits for
    // plugins to settle at it (the render thread never waits on a plugin's state).
    uint32_t sampleRate() const { return rate_; }
    void setRate(uint32_t rate);

    const ProjectStore &store() const { return store_; }
    audio::AudioSystem &audio() { return audio_; }
    size_t trackCount() const { return store_.project().tracks.size(); }

private:
    struct TapBuffers {
        std::vector<float> left, right;
    };

    ProjectStore store_;
    env::RealtimePool workers_; // the engine's render threads (normal priority)
    audio::AudioSystem audio_;  // deviceless: renders on demand
    audio::MidiHub *midi_ = nullptr; // owned by audio_
    std::unique_ptr<AudioBridge> bridge_;
    InstrumentLibrary &library_;
    AudioPool &sounds_;
    plugin::PluginManager *plugins_;
    std::vector<TapBuffers> taps_;
    uint32_t rate_;
    bool ready_ = false;
};

// One output of a pass: a file of the master output, or of some tracks summed, cut to a range.
struct PassOutput {
    int id = 0;
    bool master = true;          // the engine's master output; else the listed tracks summed
    std::vector<int> tracks;     // track indices, summed (when not master)
    bool respectMuteSolo = true; // muted tracks leave out, and with a solo only soloed ones count
    uint64_t from = 0;           // the range, in frames from the pass's start
    uint64_t to = 0;             // (exclusive)
    uint64_t tail = 0;           // at most this many frames after `to`; ends sooner once quiet
    std::string path;            // its temp file (deleted when the pass is cancelled or fails)
};

// The settings one pass renders with. Every output in a pass shares the rate, the tap kind and
// whether tracks are unmuted: a pass that needs another rate or tap is a pass of its own.
// Outputs that don't respect mute and solo need `unmuted`; the others need it false.
struct RenderPassConfig {
    uint32_t sampleRate = 48000;
    double startBeat = 0.0;
    bool preFader = false;
    bool unmuted = false;
    std::vector<PassOutput> outputs;
};

// What a finished output measured, of the samples as they were written
struct PassResult {
    int id = 0;
    std::string path;
    uint64_t frames = 0;               // written, per channel
    double integratedLufs = 0.0;       // EBU R128, -inf for silence
    double loudnessRange = 0.0;        // LU
    double shortTermMax = 0.0;         // LUFS
    std::vector<double> shortTerm;     // short-term loudness every 0.5 s, LUFS
    double truePeakDb = 0.0;           // dBTP
    float samplePeak = 0.0f;           // linear
    uint64_t clipped = 0;              // samples beyond full scale
    std::string error;
};

// One render of a set of outputs, from the engine's start to the last output's tail. It runs on
// the calling thread (a background job's), which must be the one that owns the engine. Each block
// it renders the master output or each tapped track, sums what each output takes, and appends its
// range and tail to its temp file as 32-bit float, measuring as it goes. Nothing is held whole in
// memory: each output is only a block's worth.
class RenderPass {
public:
    RenderPass(OfflineEngine &engine, RenderPassConfig config);

    RenderPass(const RenderPass &) = delete;
    RenderPass &operator=(const RenderPass &) = delete;

    // Renders until every output is finished, or the pass is cancelled (checked every block) or
    // fails. Returns true when it completed; on false, `error` says why, and the temp files are gone.
    bool run(std::string &error);
    void cancel() { cancel_.store(true, std::memory_order_relaxed); }
    bool cancelled() const { return cancelled_.load(std::memory_order_relaxed); }
    // Frames done over the pass's length, 0 to 1 (any thread)
    float fraction() const { return fraction_.load(std::memory_order_relaxed); }
    // Frames the engine rendered in total (for timing)
    uint64_t renderedFrames() const { return rendered_; }
    // One result per output, in the order given (complete after a successful run)
    const std::vector<PassResult> &results() const { return results_; }

private:
    OfflineEngine &engine_;
    RenderPassConfig config_;
    std::atomic<bool> cancel_{false};
    std::atomic<bool> cancelled_{false};
    std::atomic<float> fraction_{0.0f};
    uint64_t rendered_ = 0;
    std::vector<PassResult> results_;
};

// What one output reports when it is finished, or why it has no file. The loudness and peaks are
// measured on the file as it was written (after gain, fades, limiter, mono and dither), so they
// are what a listener gets.
struct TargetReport {
    std::string target;           // the target's name (an EachTrack output adds " - <track>")
    std::string path;             // the file written ("" when none)
    std::string error;            // why it failed or was skipped
    std::string note;             // what was changed to make it: a name taken by an earlier output of the job
    bool ok = false;
    bool midi = false;            // a MIDI file: no audio figures
    uint64_t bytes = 0;
    double seconds = 0.0;         // the file's length
    double integratedLufs = 0.0;  // -infinity for silence
    double loudnessRange = 0.0;   // LU
    double shortTermMax = 0.0;    // LUFS
    std::vector<float> shortTerm; // short-term loudness every 0.5 s, LUFS
    double truePeakDb = 0.0;      // dBTP
    double samplePeakDb = 0.0;    // dBFS
    double gainDb = 0.0;          // the gain the finishing applied
    uint64_t clipped = 0;         // samples beyond full scale before the encoder clamps them
};

// The results of one RenderJob that its background jobs write: one report slot per output, the
// cancel flag they check, and how many outputs have reported. Shared, so a job still running when
// its RenderJob is destroyed writes into memory that is still there.
struct RenderResults {
    std::mutex mutex;
    std::vector<TargetReport> reports;
    std::atomic<bool> cancel{false};
    std::atomic<size_t> finished{0};
};

// A pass's render, owned with its config by the job that runs it (defined in RenderJob.cpp)
struct RenderPassRun;

// One render of an export preset, from its targets to its files. Driven by update(), once a frame
// on the main thread, until finished():
//   Preparing   the engine for the next pass is being prepared (or waiting for plugins and samples)
//   Rendering   a pass runs on a Background job (the engine belongs to that job meanwhile)
//   Finalizing  every pass is done; the files are being finished on Background jobs
//   Done        every output has its file (its report says so)
//   Failed      some output failed (the reports say why); the rest are finished
//   Cancelled   cancel() was called, and the jobs have let go
// The outputs of a pass are grouped by sample rate, tap and mute rule, so a pass is one render
// through the engine. Each audio output is finished on its own job as soon as its pass ends, so it
// can run while the next pass renders. MIDI outputs are written straight from the project snapshot.
// The job's temp folder (<temp>/Vibin export/<id>) is removed when it finishes.
class RenderJob {
public:
    enum State { Preparing, Rendering, Finalizing, Done, Failed, Cancelled };

    // `outputFolder` is where a target without a folder of its own goes (beside the saved project);
    // empty means the library's Exports folder. `id` names the temp folder.
    RenderJob(env::Environment &env, plugin::PluginManager *plugins, InstrumentLibrary &library, AudioPool &pool,
              Project snapshot, ExportPreset preset, float masterGain, std::string outputFolder, uint64_t id);
    // Waits for the render and finishing jobs to let go (cancelling them first when still running),
    // then frees the engine on this thread
    ~RenderJob();

    RenderJob(const RenderJob &) = delete;
    RenderJob &operator=(const RenderJob &) = delete;

    // Main thread, once a frame. Starts the job the first time, prepares engines, starts passes and
    // finishing jobs, and reaches the terminal state when they have all finished.
    void update();
    // Stops it: a render stops within a block and the finishing jobs at their next chunk. Returns at
    // once; the job is Cancelled in a later update() once its jobs have let go. A job not yet
    // started is Cancelled straight away.
    void cancel();

    bool started() const { return started_; }
    bool finished() const { return state_ == Done || state_ == Failed || state_ == Cancelled; }
    State state() const { return state_; }
    // 0 to 1 (any thread, from the last update): the renders are 80% of the work, the finishing 20%
    float fraction() const;
    // What the job is doing now, for the panel
    const std::string &status() const { return status_; }
    // One report per output, in the order the targets name them (a copy; any thread)
    std::vector<TargetReport> reports() const;
    const std::string &presetName() const { return preset_.name; }
    uint64_t id() const { return id_; }
    const std::string &tempFolder() const { return tempFolder_; }
    // Seconds since the job started (to its end, once finished)
    double elapsedSeconds() const;

private:
    // An output as the targets expand to: one file, of the master or of some tracks, or a MIDI file
    struct PlannedOutput {
        ExportTarget target;
        std::string name;
        std::vector<int> tracks;  // the tracks summed (or a MIDI file's; empty: every instrument track)
        bool master = false;
        bool midi = false;
        bool skipped = false;     // an error to report; nothing is rendered
        double startBeat = 0.0;
        double endBeat = 0.0;
        uint64_t from = 0;        // the range and tail, in frames from its pass's start
        uint64_t to = 0;
        uint64_t tail = 0;
        int pass = -1;
        std::string path;         // the file written
        std::string tempPath;     // the render's float file (audio)
        std::string note;         // set when its name was taken by an earlier output of the job
    };
    // A pass: one rate, tap and mute rule, and the outputs it renders
    struct PlannedPass {
        uint32_t rate = 48000;
        bool preFader = false;
        bool unmuted = false;
        double startBeat = 0.0;
        std::vector<size_t> outputs;
    };

    void makeTempFolder();
    void removeTemp();
    void plan();
    void startPass();
    void collectPass();
    void startFinalize(size_t output, const PassResult &result);
    void startMidi(size_t output);
    void failOutput(size_t output, const std::string &error);
    void finish(State state);
    bool anyFailed() const;

    env::Environment &env_;
    plugin::PluginManager *plugins_;
    InstrumentLibrary &library_;
    AudioPool &pool_;
    Project snapshot_;
    ExportPreset preset_;
    float masterGain_;
    std::string outputFolder_;
    uint64_t id_;
    std::string tempFolder_;
    State state_ = Preparing;
    bool started_ = false;
    bool cancelRequested_ = false;
    std::string status_ = "Getting ready";
    std::chrono::steady_clock::time_point enqueued_;
    std::chrono::steady_clock::time_point startedAt_;
    std::chrono::steady_clock::time_point finishedAt_;
    std::vector<PlannedOutput> outputs_;
    std::vector<PlannedPass> passes_;
    size_t nextPass_ = 0;             // the pass being prepared, rendered or next
    bool passRunning_ = false;
    std::shared_ptr<RenderPassRun> run_;
    std::shared_ptr<RenderResults> results_;
    std::unique_ptr<OfflineEngine> engine_; // main thread; its render job uses it only while running
    env::JobGroup renderJobs_;   // declared after the engine, so they're waited for before it goes
    env::JobGroup finalizeJobs_;
};

// The render queue: presets queued by Export, one rendering at a time. Main thread only.
class ExportQueue {
public:
    ExportQueue(env::Environment &env, plugin::PluginManager *plugins, InstrumentLibrary &library, AudioPool &pool);
    ~ExportQueue();

    ExportQueue(const ExportQueue &) = delete;
    ExportQueue &operator=(const ExportQueue &) = delete;

    // Queues a render of a preset of the project as it is now. Returns the job's id.
    uint64_t enqueue(Project snapshot, ExportPreset preset, float masterGain, std::string outputFolder = {});
    // Every job, queued, running and finished, in the order they were queued
    std::vector<RenderJob *> jobs() const;
    // The job rendering now (the first not yet finished), or null
    RenderJob *active() const;
    // Runs the active job's update (only the first unfinished job renders; the rest wait)
    void update();
    // Cancels a job (a queued one at once, a running one as RenderJob::cancel does)
    bool cancel(uint64_t id);
    // Forgets a finished job (its reports go with it); false when it isn't finished or is unknown
    bool remove(uint64_t id);

private:
    env::Environment &env_;
    plugin::PluginManager *plugins_;
    InstrumentLibrary &library_;
    AudioPool &pool_;
    std::vector<std::unique_ptr<RenderJob>> jobs_;
    uint64_t nextId_ = 1;
};

}
