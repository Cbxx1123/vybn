#pragma once

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "Daemon.h"
#include "RealtimePool.h"
#include "Semaphore.h"

namespace env {

// Where a job runs. Each lane has its own threads, so a slow download never holds up an
// instrument loading, and nothing background ever takes a real-time worker.
enum class Lane {
    Interactive, // work someone is waiting for: loading the instrument just chosen
    Background,  // the rest: decoding sound files, scanning
    Disk,        // the disk streamer's reads (see Scheduler::addPoller)
    Network,     // downloads and listings, which mostly wait
};
constexpr size_t kLaneCount = 4;
const char *laneName(Lane lane);
class Environment;
class JobGroup;
// Runs `job` on the environment's Scheduler, on `lane`, counted in `group`; or, when it has no
// Scheduler (some tests), on a thread of its own
void runJob(Environment &env, Lane lane, std::function<void()> job, JobGroup &group);

// A set of jobs that can be waited for: owners of jobs that use `this` wait in their
// destructor. Jobs dropped because the scheduler stopped count as finished.
class JobGroup {
public:
    JobGroup() = default;
    ~JobGroup() { wait(); }
    JobGroup(const JobGroup &) = delete;
    JobGroup &operator=(const JobGroup &) = delete;

    void wait();
    size_t pending() const { return pending_.load(); }

private:
    friend class Scheduler;
    friend void runJob(Environment &, Lane, std::function<void()>, JobGroup &);
    void add() { ++pending_; }
    void finish();

    std::atomic<size_t> pending_{0};
    std::mutex mutex_;
    std::condition_variable idle_;
};

struct SchedulerConfig {
    uint32_t realtimeWorkers = 0xffffffff; // audio worker threads; the default is one per core but one
    uint32_t workers = 0;                  // Interactive and Background threads; 0: half the cores, at least 2
    uint32_t diskThreads = 2;
    uint32_t networkThreads = 4;
};

// The program's central manager of threads: every thread besides the main and audio threads
// belongs to it. The real-time pool splits audio across cores; the lanes run everything else.
// Add it before the daemons that use it (the AudioSystem finds its real-time pool through it).
//
// submit() queues a job on a lane from any thread but a real-time one; Interactive jobs run
// ahead of Background ones on the shared workers. A lane's pollers run whenever one of its
// threads wakes: from wake(), which real-time threads may call (it takes no lock), or every
// `interval` ms. The disk streamer is a Disk poller, woken by the audio thread's requests.
// stop() runs the stop hooks (so jobs waiting on the network can be cancelled), drops jobs not
// yet started and joins every thread.
class Scheduler : public Daemon {
public:
    explicit Scheduler(class Environment &env, SchedulerConfig config = {});
    ~Scheduler() override;

    void submit(Lane lane, std::function<void()> job, JobGroup *group = nullptr);
    // Real-time safe: wakes one of the lane's threads to run its pollers
    void wake(Lane lane);
    // fn returns true when it did something (it is then called again at once). Returns an id.
    int addPoller(Lane lane, std::function<bool()> fn, uint32_t intervalMs = 50);
    void removePoller(int id);
    // Runs at stop(), before threads are joined
    void onStop(std::function<void()> hook) { stopHooks_.push_back(std::move(hook)); }

    RealtimePool &realtime() { return realtime_; }
    // How many audio worker threads to use (0: the audio thread alone); at most the pool's size
    void setRealtimeWorkers(uint32_t count) { realtime_.setActive(count); }
    static uint32_t cores();

    struct LaneStats {
        uint32_t threads = 0;
        uint32_t busy = 0;       // threads running a job now
        size_t queued = 0;
        uint64_t completed = 0;
    };
    LaneStats stats(Lane lane) const;

    void stop(Environment &env) override;

private:
    struct Job {
        std::function<void()> fn;
        JobGroup *group = nullptr;
    };
    struct Poller {
        int id;
        std::function<bool()> fn;
        uint32_t intervalMs;
        std::atomic<int> running{0};
    };
    struct Pool {
        std::vector<std::thread> threads;
        mutable std::mutex mutex;                   // the queues and the poller list
        std::vector<Lane> lanes;                    // the lanes it serves, first first
        std::array<std::deque<Job>, kLaneCount> queues;
        std::vector<std::shared_ptr<Poller>> pollers;
        Semaphore wake;                             // a job queued, or a poller woken
        std::atomic<uint32_t> busy{0};
        std::array<std::atomic<uint64_t>, kLaneCount> completed{};
    };

    Pool &poolFor(Lane lane);
    const Pool &poolFor(Lane lane) const;
    void startPool(Pool &pool, uint32_t threads, std::vector<Lane> lanes, const char *name);
    void loop(Pool &pool);
    bool runPollers(Pool &pool);
    void shutdown();

    RealtimePool realtime_;
    Pool workers_; // Interactive and Background
    Pool disk_;
    Pool network_;
    std::atomic<bool> stopping_{false};
    std::vector<std::function<void()>> stopHooks_;
    int nextPoller_ = 1;
};

}
