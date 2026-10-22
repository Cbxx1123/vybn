#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "Daemon.h"
#include "EventBus.h"
#include "PostQueue.h"

namespace env {

// How long a daemon's ticks take, for status displays. Busy time leaves out what the daemon
// reported as waiting (Daemon::reportWait).
struct DaemonStats {
    const Daemon *daemon = nullptr;
    double lastMs = 0.0;    // busy time of the most recent tick
    double averageMs = 0.0; // busy time, smoothed over roughly the last second
    double peakMs = 0.0;    // recent highest busy time, falling back slowly
    double waitMs = 0.0;    // time spent blocked per tick, smoothed
};

// The program's runtime: owns the daemons and runs the main loop that ticks them.
//
//   env::Environment env;
//   auto &windows = env.add<engine::WindowSystem>();
//   auto &ui = env.add<ui::UISystem>("My app", 1280, 800);
//   ...build the UI...
//   env.run(); // until something calls requestStop (closing the main window does)
//
// Each loop iteration: run posted actions, dispatch posted events, then tick every daemon in
// the order they were added.
class Environment {
public:
    Environment() = default;
    ~Environment();

    Environment(const Environment &) = delete;
    Environment &operator=(const Environment &) = delete;

    // Constructs a daemon as T(*this, args...) and takes ownership of it. Add daemons before
    // run(), dependencies first.
    template <typename T, typename... Args>
    T &add(Args &&...args)
    {
        auto daemon = std::make_unique<T>(*this, std::forward<Args>(args)...);
        T &ref = *daemon;
        daemons_.push_back(std::move(daemon));
        stats_.push_back({&ref, 0.0, 0.0, 0.0, 0.0});
        return ref;
    }

    // The daemon of type T, or nullptr
    template <typename T>
    T *find() const
    {
        for (const auto &d : daemons_) {
            if (auto *t = dynamic_cast<T *>(d.get()))
                return t;
        }
        return nullptr;
    }

    // The daemon of type T; throws if it hasn't been added. For a daemon's constructor to find
    // what it depends on.
    template <typename T>
    T &require(const char *by = "") const
    {
        if (T *t = find<T>())
            return *t;
        throw std::runtime_error(std::string("Environment: a daemon needed by ") + by + " has not been added");
    }

    EventBus &events() { return events_; }

    // Runs `action` on the main thread at the start of the next loop iteration. Thread-safe,
    // but takes a lock, so not for real-time threads. It also calls the wake hook, so a loop
    // that is sleeping wakes up.
    void post(std::function<void()> action);
    // Called (from any thread, after queueing) by post and by the event bus's post, so the main
    // loop can sleep between frames and still handle them at once. The WindowSystem installs one
    // that wakes the window's event wait. The hook must be thread-safe; null removes it.
    void setWakeHook(std::function<void()> hook);

    // Starts every daemon, loops until requestStop, then stops them in reverse order
    void run();
    void requestStop() { stopRequested_ = true; }
    bool running() const { return running_; }

    uint64_t frame() const { return frame_; }
    const std::vector<DaemonStats> &stats() const { return stats_; }
    size_t daemonCount() const { return daemons_.size(); }
    const Daemon &daemon(size_t index) const { return *daemons_[index]; }

private:
    std::vector<std::unique_ptr<Daemon>> daemons_;
    std::vector<DaemonStats> stats_;
    EventBus events_;
    PostQueue posted_; // actions from post()
    bool stopRequested_ = false;
    bool running_ = false;
    uint64_t frame_ = 0;
};

}
