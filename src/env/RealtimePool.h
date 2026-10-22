#pragma once

#include <atomic>
#include <cstdint>
#include <thread>
#include <vector>

#include "Semaphore.h"

namespace env {

// Worker threads for real-time work split across cores: the audio thread hands a batch of
// independent jobs (one per track, say) to run(), which runs them on the workers and on the
// calling thread together and returns when all are done.
//
// run() never locks or allocates. Jobs are claimed one at a time from a single atomic ticket
// (the batch's number, its size and the next job, packed together), so a worker waking late
// can't take a job from the wrong batch, and the caller takes any job no worker has reached yet:
// it only ever waits for jobs already under way. Workers run at a raised priority, with denormals
// flushed, as the audio thread does.
//
// Between batches a worker sleeps on a semaphore, and run() wakes only workers that are asleep
// (it counts them), so a batch makes no kernel call when its workers are already awake. When
// batches come quickly (closer together than kSpinWindowNs, a quarter of a millisecond: an offline render calling back to
// back), a worker that finishes one spins for the next instead of sleeping, so it starts at once
// without being woken. At the live audio thread's pace (a block every few milliseconds) batches
// are far apart, so workers sleep straight away and spin never.
class RealtimePool {
public:
    using JobFn = void (*)(void *context, uint32_t index);

    // Workers run at time-critical priority unless `realtimePriority` is false (an offline engine's
    // pool, which shouldn't outrank the live audio thread). Denormals are flushed either way.
    explicit RealtimePool(bool realtimePriority = true) : realtimePriority_(realtimePriority) {}
    ~RealtimePool() { stop(); }
    RealtimePool(const RealtimePool &) = delete;
    RealtimePool &operator=(const RealtimePool &) = delete;

    // Starts `workers` threads (0 runs everything on the caller). Not while run() is running.
    void start(uint32_t workers);
    // Runs `fn` on each worker as it starts (before it takes any job): for per-thread state, such
    // as an offline engine marking its workers as rendering offline. Set before start().
    using WorkerStartFn = void (*)();
    void onWorkerStart(WorkerStartFn fn) { workerStart_ = fn; }
    void stop();
    uint32_t workers() const { return static_cast<uint32_t>(threads_.size()); }
    // How many of the workers batches use (at most workers()); any thread
    void setActive(uint32_t count) { active_.store(count, std::memory_order_relaxed); }
    uint32_t active() const;

    // Runs fn(context, i) for every i below count and returns when all have finished. One
    // caller at a time (the audio thread, or an offline render on the main thread).
    void run(uint32_t count, JobFn fn, void *context);

    // Jobs run on workers rather than the caller, since start (for display)
    uint64_t stolen() const { return stolen_.load(std::memory_order_relaxed); }
    uint64_t batches() const { return batches_.load(std::memory_order_relaxed); }

private:
    static constexpr uint64_t kIndexBits = 20;
    static constexpr uint64_t kCountBits = 20;
    static constexpr uint64_t kMask = (1ull << kIndexBits) - 1;

    // Claims and runs jobs of the batch in `ticket_` until none are left; returns how many
    uint32_t work();
    void loop(uint32_t index);
    // Spins up to kSpinWindowNs for a batch with jobs left to claim; whether one came (or a stop)
    bool spinForBatch() const;

    std::vector<std::thread> threads_;
    bool realtimePriority_ = true;
    WorkerStartFn workerStart_ = nullptr;
    Semaphore wake_;
    std::atomic<bool> stopping_{false};
    std::atomic<uint32_t> active_{0};

    // The batch: written before its ticket is published, stable until it is done
    JobFn fn_ = nullptr;
    void *context_ = nullptr;
    alignas(64) std::atomic<uint64_t> ticket_{0}; // batch << 40 | count << 20 | next
    alignas(64) std::atomic<uint32_t> done_{0};
    uint64_t batch_ = 0;
    std::atomic<uint64_t> stolen_{0};
    std::atomic<uint64_t> batches_{0};

    // Waking: workers asleep (or about to be) on wake_, which run() takes from as it wakes them
    // (see run and loop for why no wake is lost), and how long ago the batch before the latest
    // one began, which says whether batches come quickly enough to spin for
    static constexpr int64_t kSpinWindowNs = 250000;
    alignas(64) std::atomic<uint32_t> sleeping_{0};
    std::atomic<int64_t> gapNs_{INT64_MAX};
    int64_t lastRunNs_ = 0; // the caller's
};

}
