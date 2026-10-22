#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "AudioProcessor.h"
#include "DeviceChain.h"
#include "Routing.h"
#include "Transport.h"
#include "env/Daemon.h"
#include "env/Environment.h"
#include "env/RealtimePool.h"
#include "env/SpscQueue.h"

namespace audio {

struct DeviceInfo {
    std::string name;
    bool isDefault = false;
};

struct AudioSettings {
    int device = -1;             // index into AudioSystem::devices(), or -1 for the system default
    uint32_t sampleRate = 0;     // 0 for the device's own rate
    uint32_t bufferFrames = 256; // frames per callback (a request; the device may choose otherwise)
    // Takes the device for this program alone, for the lowest latency. WASAPI's shared mode
    // usually won't go below about 10 ms. Falls back to shared if the device refuses.
    bool exclusive = false;
    // The audio input (microphone, audio interface): kNoInput for none, -1 for the system
    // default, else an index into AudioSystem::inputDevices(). With one, the device opens for
    // input and output together, and processors get the input's channels in AudioBlock::inputs.
    int input = kNoInput;
    static constexpr int kNoInput = -2;
};

// Events published on the Environment's bus, from the main thread
struct AudioDeviceStarted {
    std::string device;
    uint32_t sampleRate;
    uint32_t bufferFrames;
};
struct AudioDeviceStopped {
    std::string reason; // empty when stopped on request
};

// Tag for an AudioSystem with no audio device: an offline engine (exports, tests) that renders its
// graph on demand with renderOffline. It never opens a device, and its processors are its own.
struct Deviceless {};

// The audio daemon: owns the audio device (miniaudio; WASAPI on Windows) and the graph of
// AudioProcessors it plays, and runs the real-time audio thread.
//
// Threads: everything here is for the main thread, except where noted. The device's callback
// runs on miniaudio's audio thread. It takes processor additions and removals from a lock-free
// queue, runs the transport, mixes every processor into the master bus, applies the master
// gain, and records peak levels and load in atomics. It never locks or allocates. Processors it
// has dropped come back to the main thread through a second queue and are deleted in tick().
//
// The device opens in start() (when the Environment starts running). If no device is available,
// the system stays stopped, with the reason in lastError(), and the rest of the program runs on.
class AudioSystem : public env::Daemon {
public:
    static constexpr uint32_t kChannels = 2;      // stereo output
    static constexpr uint32_t kMaxBlock = 4096;   // longer callbacks are processed in pieces
    static constexpr uint32_t kMaxInputs = 8;     // input channels beyond this are ignored

    explicit AudioSystem(env::Environment &env, AudioSettings settings = {});
    // No device at all (see Deviceless): devices() is empty, start() opens nothing, and the graph,
    // the transport, the master gain and the master effects work with renderOffline. It renders
    // its tracks on one thread until setPool gives it one.
    AudioSystem(env::Environment &env, Deviceless);
    ~AudioSystem() override;

    // The graph. add constructs a processor, prepares it and sends it to the audio thread; the
    // AudioSystem owns it. remove takes it out and deletes it once the audio thread lets go.
    template <typename T, typename... Args>
    T &add(Args &&...args)
    {
        auto processor = std::make_unique<T>(std::forward<Args>(args)...);
        T &ref = *processor;
        adopt(std::move(processor));
        return ref;
    }
    void remove(AudioProcessor &processor);
    size_t processorCount() const { return owned_.size(); }
    // Where the tracks' sound goes (sends, groups): sent to the audio thread in order with add and
    // remove, so a routing must name only processors in the graph, and must be replaced before one it
    // names is removed (removing one the current routing names drops the routing, as a safety).
    // nullptr: every track straight into the master, as with none. Finished (Routing::finish) and
    // deleted here once replaced.
    void setRouting(std::unique_ptr<Routing> routing);
    // Main thread, while nothing is rendering: the pool tracks render on (nullptr for one thread).
    // The scheduler's pool is the live audio thread's (one caller at a time), so an offline engine
    // gives its own. A deviceless system doesn't use the scheduler's pool until it is given one.
    void setPool(env::RealtimePool *pool) { pool_ = pool; }
    // Main thread: deletes the processors the audio thread has let go of (tick() does this too;
    // a deviceless system, which has no tick work, calls it itself)
    void collectOffline();

    // Devices: outputs and inputs
    const std::vector<DeviceInfo> &devices() const { return devices_; }
    const std::vector<DeviceInfo> &inputDevices() const { return inputDevices_; }
    void refreshDevices();
    // Settings for the next start, or a restart now if the device is running
    void configure(const AudioSettings &settings);
    // Reopens the device with new settings. Returns whether it is running afterwards.
    bool restart(const AudioSettings &settings);
    void stopDevice();
    bool running() const { return running_; }
    const AudioSettings &settings() const { return settings_; }
    const std::string &deviceName() const { return deviceName_; }
    const std::string &backendName() const { return backendName_; }
    const std::string &lastError() const { return lastError_; }
    bool exclusive() const { return exclusive_; } // whether the running device is in exclusive mode
    // The open input: its name and channel count (0 when there is none)
    const std::string &inputName() const { return inputName_; }
    uint32_t inputChannels() const { return inputChannels_; }
    // Roughly how late recorded input arrives against what was playing: the input's and the
    // output's buffers. Recordings are moved earlier by this much.
    uint32_t roundTripFrames() const { return roundTripFrames_; }
    uint32_t sampleRate() const { return sampleRate_; }
    uint32_t bufferFrames() const { return bufferFrames_; }
    // One buffer's worth of time, the least latency the output adds
    double bufferMs() const { return sampleRate_ > 0 ? 1000.0 * bufferFrames_ / sampleRate_ : 0.0; }

    Transport &transport() { return transport_; }
    AudioParam &masterGain() { return masterGain_; } // linear, 0-1.5
    // The project's master volume (linear, 0-2), after the master effects and before the master gain:
    // part of the mix, so offline engines (exports) play it too, while the master gain is monitoring
    AudioParam &masterVolume() { return masterVolume_; }
    // Effects on the master bus, before the master gain (set with apply(), collect() once a frame)
    DeviceChain &masterEffects() { return masterEffects_; }
    // While the device is stopped: prepares every processor for offline renders at `sampleRate`
    // now, rather than at the first render (so plugins can be waited for to settle at it)
    void prepareOffline(double sampleRate);

    // Meters, written by the audio thread. takePeak returns the highest absolute sample on a
    // channel since the last call (main thread only: it resets the value).
    float takePeak(uint32_t channel);
    float takeInputPeak(uint32_t channel); // likewise for an input channel
    float load() const { return load_.load(std::memory_order_relaxed); } // processing time / buffer time, smoothed
    uint64_t overloads() const { return overloads_.load(std::memory_order_relaxed); } // callbacks that ran too long
    uint64_t callbacks() const { return callbacks_.load(std::memory_order_relaxed); }

    // Renders `frames` frames of interleaved stereo into `out` on the calling thread, through
    // the same path the device uses. Only while the device is stopped (returns false otherwise).
    // For tests, and exporting the mix (daw::OfflineEngine).
    // `input` is interleaved input for the same frames (`inputChannels` of them), or null.
    bool renderOffline(float *out, uint32_t frames, double sampleRate = 48000.0, const float *input = nullptr,
                       uint32_t inputChannels = 0);

    void start(env::Environment &env) override;
    void tick(env::Environment &env) override;
    void stop(env::Environment &env) override;

private:
    struct Impl; // miniaudio state, kept out of this header
    struct Command {
        enum class Type { Add, Remove, Route } type = Type::Add;
        AudioProcessor *processor = nullptr;
        const Routing *routing = nullptr; // Route
        uint64_t sequence = 0;            // Route: which setRouting it was
    };

    void adopt(std::unique_ptr<AudioProcessor> processor);
    void reserveBuffers();
    bool openDevice();
    void prepareAll(double sampleRate);
    // Applies queued commands directly, when no audio thread is consuming them
    void applyCommandsHere();
    void deleteRetired();
    void deleteRoutings();
    void renderRouted(AudioBlock &block);

    // Audio thread (or offline)
    static void callback(void *device, void *output, const void *input, uint32_t frames);
    void render(float *out, uint32_t frames, double sampleRate, const float *input, uint32_t inputChannels);
    void applyCommands();
    struct ParallelBlock {
        AudioProcessor *const *processors;
        const AudioBlock *block;
    };
    static void renderJob(void *context, uint32_t index)
    {
        auto *job = static_cast<ParallelBlock *>(context);
        job->processors[index]->renderParallel(*job->block);
    }
    struct RoutedBlock {
        const Route *routes;
        const AudioBlock *block;
    };
    static void routedJob(void *context, uint32_t index);

    env::Environment &env_;
    std::unique_ptr<Impl> impl_;
    AudioSettings settings_;
    std::vector<DeviceInfo> devices_;
    std::vector<DeviceInfo> inputDevices_;
    std::string deviceName_;
    std::string inputName_;
    uint32_t inputChannels_ = 0;
    uint32_t roundTripFrames_ = 0;
    std::string backendName_;
    std::string lastError_;
    uint32_t sampleRate_ = 0;
    uint32_t bufferFrames_ = 0;
    bool running_ = false;
    bool exclusive_ = false;
    bool deviceless_ = false;
    std::atomic<bool> stoppedUnexpectedly_{false};

    // Main thread: every processor, alive until the audio thread has let go of it
    std::vector<std::unique_ptr<AudioProcessor>> owned_;
    // Main thread: the routings sent, until the audio thread has moved past them
    struct SentRouting {
        uint64_t sequence;
        std::unique_ptr<const Routing> routing;
    };
    std::vector<SentRouting> routings_;
    uint64_t routingsSent_ = 0;
    std::atomic<uint64_t> routingApplied_{0}; // the last setRouting the audio thread took
    env::SpscQueue<Command, 256> commands_;          // main -> audio
    env::SpscQueue<AudioProcessor *, 256> retired_;  // audio -> main, ready to delete

    // Audio thread state
    std::vector<AudioProcessor *> active_; // reserved up front, so adding never allocates
    std::vector<AudioProcessor *> parallel_; // the active ones that render in parallel
    const Routing *routing_ = nullptr;       // what the tracks play through (null: each into the master)
    env::RealtimePool *pool_ = nullptr;      // the scheduler's, when there is one
    std::array<std::vector<float>, kChannels> scratch_;
    std::array<std::vector<float>, kMaxInputs> inputScratch_;
    // The master effects' sidechain keys (Routing::masterKeys), read from the tracks once they have rendered
    static constexpr uint32_t kMaxMasterKeys = kMaxKeys;
    std::array<std::array<std::vector<float>, kChannels>, kMaxMasterKeys> masterKey_;
    Smoothed gain_;
    Smoothed volume_;
    double preparedRate_ = 0.0;

    Transport transport_;
    AudioParam masterGain_{0.8f, 0.0f, 1.5f};
    AudioParam masterVolume_{1.0f, 0.0f, 2.0f};
    DeviceChain masterEffects_;
    std::array<std::atomic<float>, kChannels> peaks_{};
    std::array<std::atomic<float>, kMaxInputs> inputPeaks_{};
    std::atomic<float> load_{0.0f};
    std::atomic<uint64_t> overloads_{0};
    std::atomic<uint64_t> callbacks_{0};
};

}
