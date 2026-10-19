#pragma once

#include <atomic>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "Protocol.h"
#include "Sandbox.h"

namespace plugin {

// What an instance is doing
enum class Status {
    Ready,   // loaded; processing once activated
    Crashed, // its host process ended
    Hung,    // it stopped answering and was stopped
    Closed,  // shut down by the app
};

// One block for a plugin: planar audio in and out (stereo or mono), the block's timed events (in
// order) and the transport at its start
struct ProcessBlock {
    const float *const *inputs = nullptr;
    uint32_t inputChannels = 0;
    float *const *outputs = nullptr;
    uint32_t outputChannels = 0;
    uint32_t frames = 0;
    TransportInfo transport;
    const Event *events = nullptr;
    uint32_t eventCount = 0;
};

// The app's side of one plugin running in its own sandboxed host process.
//
// Threads:
// - create, scan and the control calls (activate, params, state...) block for up to a few
//   seconds while the host answers: call them from a job, not the audio thread. They take a lock,
//   so one runs at a time.
// - setParam, poll and the take* calls are for the main thread.
// - process is for the audio thread: it never locks or allocates. It hands the block to the host
//   and waits for it until the block's deadline (most of its duration; much longer while
//   rendering offline). A late block plays silence, and until the host catches up later blocks
//   don't wait at all. A host that stays late for `hangSeconds` is stopped by poll() and reported
//   as hung; one that ends is reported as crashed. Either way the app goes on.
class Instance {
public:
    ~Instance();
    Instance(const Instance &) = delete;
    Instance &operator=(const Instance &) = delete;

    // Starts a sandboxed host and makes `plugin` in it. Null with `error` on failure.
    static std::unique_ptr<Instance> create(const Descriptor &plugin, const SandboxOptions &options, std::string &error);
    // Lists the plugins in a .clap file, loading it in a sandboxed host of its own
    static bool scan(const std::string &path, const SandboxOptions &options, std::vector<Descriptor> &found, std::string &error);

    const Descriptor &descriptor() const { return descriptor_; }
    const LoadedInfo &info() const { return info_; }
    // Whether the host is inside the AppContainer, and if not why
    bool contained() const { return process_.contained(); }
    const std::string &sandboxNote() const { return process_.note(); }

    // Control (not the audio thread)
    bool activate(double sampleRate, std::string &error);
    void deactivate();
    bool active() const { return active_.load(); }
    bool params(std::vector<ParamInfo> &out, std::string &error);
    bool paramText(uint32_t id, double value, std::string &text);
    bool saveState(std::string &state, std::string &error, uint32_t timeoutMs = 10000);
    bool loadState(const std::string &state, std::string &error);
    // The plugin's own window (info().gui): opened in front, or closed
    bool openGui(std::string &error);
    void closeGui();
    // Its interface drawn into the app instead (info().embedded): opened at its size
    bool openEmbeddedGui(uint32_t &width, uint32_t &height, std::string &error);
    bool guiEmbedded() const { return guiEmbedded_.load(); }
    // The latest picture of the embedded interface, when newer than `sequence` (which it updates):
    // its size and rows of 4-byte pixels. Any thread but the audio thread.
    bool readGuiFrame(std::vector<uint8_t> &pixels, uint32_t &width, uint32_t &height, uint32_t &sequence);
    // The app's mouse and keys for the embedded interface, and a new size for it: sent at once,
    // with no answer (never waiting behind a control call)
    void sendGuiInput(const GuiInput &input);
    void resizeGui(uint32_t width, uint32_t height);
    // Where the app draws the interface on the desktop (or that it isn't shown): the plugin's
    // window goes under it, so its own menus and dialogs open there
    void placeGui(int32_t x, int32_t y, bool shown, void *appWindow);
    // Lets the host bring a window of its own to the front (only the program in front may)
    void allowToFront();
    // The part of the interface the app shows, in its pixels (a zero width: all of it)
    void clipGui(int32_t x, int32_t y, int32_t width, int32_t height);
    // Whether its window is open (it says when the user closes it)
    bool guiOpen() const { return guiOpen_.load(); }
    // The host's process id (tests, and letting its window come to the front)
    uint32_t hostProcessId() const;
    // Ends the host: politely, then by force
    void close();

    // Main thread: a parameter change, applied at the start of the next block (or at once while
    // inactive). False when the ring is full.
    bool setParam(uint32_t id, double value);
    // Main thread, once a frame: the watchdog, and the host's news
    void poll();
    // The plugin's own parameter changes since the last call
    std::vector<ParamChange> takeChanges();
    // What the plugin logged since the last call
    std::vector<std::string> takeLog();
    // Whether its parameter list or values changed (it asked for a rescan), or its state did in a
    // way parameters don't show, since the last call
    bool takeParamsChanged() { return paramsChanged_.exchange(false); }
    bool takeStateDirty() { return stateDirty_.exchange(false); }

    Status status() const { return status_.load(std::memory_order_acquire); }
    // Why it isn't Ready: "crashed (an access violation)", "stopped responding"
    std::string statusText() const;
    uint32_t latency() const;

    // Audio thread. Writes the outputs (silence when the plugin couldn't answer in time, isn't
    // active or has failed); returns whether they are the plugin's.
    bool process(const ProcessBlock &block);
    // Any thread: rendering offline (exports) waits for the plugin as long as it takes, up to a limit
    void setOffline(bool offline) { offline_.store(offline); }
    // Blocks that played silence because the plugin was late
    uint32_t missed() const { return missed_.load(std::memory_order_relaxed); }

    // How long a plugin may stay late before it counts as hung
    double hangSeconds = 2.0;

private:
    Instance() = default;
    bool start(const SandboxOptions &options, std::string &error);
    // Sends a request and waits for its reply, handling news meanwhile. Locks controlMutex_.
    bool call(Message type, const std::string &payload, std::string &reply, uint32_t timeoutMs, std::string &error);
    // Reads every whole message waiting (controlMutex_ held); stops at `sequence`'s reply
    bool readMessages(uint32_t sequence, bool &replied, bool &failed, std::string &reply);
    void handleNews(const MessageHeader &header, const std::string &payload);
    bool alive() const;

    Descriptor descriptor_;
    LoadedInfo info_;
    HostProcess process_;
    void *toHost_ = nullptr;   // pipe the app writes
    void *fromHost_ = nullptr; // pipe the app reads
    void *mapping_ = nullptr;
    void *processEvent_ = nullptr;
    void *doneEvent_ = nullptr;
    void *timer_ = nullptr; // the audio thread's deadline
    Shared *shared_ = nullptr;

    std::mutex controlMutex_;
    uint32_t sequence_ = 0;
    std::atomic<Status> status_{Status::Ready};
    std::atomic<uint32_t> exitCode_{0};
    std::atomic<uint32_t> latency_{0};
    std::atomic<bool> paramsChanged_{false};
    std::atomic<bool> stateDirty_{false};
    std::atomic<bool> guiOpen_{false};
    std::atomic<bool> guiEmbedded_{false};
    std::mutex writeMutex_;        // one message at a time on the pipe
    void *guiMapping_ = nullptr;   // the embedded interface's frame
    GuiFrame *guiFrame_ = nullptr;
    void *guiWindow_ = nullptr; // the host's frame window, placed and stacked from here
    bool send(Message type, uint32_t sequence, const std::string &payload);
    std::mutex newsMutex_;
    std::deque<std::string> log_;
    std::vector<ParamChange> changes_;

    // The audio side
    std::atomic<bool> active_{false};
    std::atomic<bool> inProcess_{false}; // the audio thread is in process() (a handshake with deactivate)
    std::atomic<bool> offline_{false};
    double sampleRate_ = 48000.0;
    int64_t ticksPerSecond_ = 1;
    uint32_t request_ = 0;              // audio thread: the last request sent
    bool late_ = false;                 // audio thread: that request hasn't been answered
    std::atomic<int64_t> lateSince_{0}; // when the host fell behind (0 while it keeps up)
    std::atomic<uint32_t> missed_{0};
};

}
