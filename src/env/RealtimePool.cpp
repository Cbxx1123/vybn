#include "RealtimePool.h"

#include <algorithm>
#include <chrono>

#if defined(__SSE__) || defined(_M_X64) || defined(__x86_64__)
#include <xmmintrin.h>
#define ENV_HAS_SSE 1
#endif
#ifdef _WIN32
#include <windows.h>
#endif

namespace env {

namespace {

void pause()
{
#ifdef ENV_HAS_SSE
    _mm_pause();
#else
    std::this_thread::yield();
#endif
}

int64_t nowNs()
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

}

void RealtimePool::start(uint32_t workers)
{
    stop();
    stopping_ = false;
    workers = std::min<uint32_t>(workers, 63);
    active_ = workers;
    gapNs_ = INT64_MAX;
    lastRunNs_ = 0;
    for (uint32_t i = 0; i < workers; ++i)
        threads_.emplace_back([this, i] { loop(i); });
}

void RealtimePool::stop()
{
    if (threads_.empty())
        return;
    stopping_ = true;
    wake_.release(static_cast<uint32_t>(threads_.size()));
    for (std::thread &t : threads_)
        t.join();
    threads_.clear();
    // Wakes nobody took (workers that were spinning, not asleep) would wake the next start's
    // workers without being counted
    while (wake_.waitFor(0)) {
    }
    sleeping_ = 0;
}

uint32_t RealtimePool::active() const
{
    return std::min(active_.load(std::memory_order_relaxed), workers());
}

bool RealtimePool::spinForBatch() const
{
    const int64_t begin = nowNs();
    for (uint32_t i = 1;; ++i) {
        const uint64_t ticket = ticket_.load(std::memory_order_acquire);
        if ((ticket & kMask) < ((ticket >> kIndexBits) & kMask) || stopping_.load(std::memory_order_relaxed))
            return true;
        if ((i & 63) == 0 && nowNs() - begin > kSpinWindowNs)
            return false;
        pause();
    }
}

void RealtimePool::loop(uint32_t index)
{
#ifdef ENV_HAS_SSE
    _mm_setcsr(_mm_getcsr() | 0x8040); // flush-to-zero and denormals-are-zero, as on the audio thread
#endif
#ifdef _WIN32
    if (realtimePriority_)
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
#endif
    if (workerStart_)
        workerStart_();
    for (;;) {
        // Batches coming quickly: spin for the next one rather than sleep (workers beyond the
        // active count never do; they only run when woken)
        const bool quick = index < active() && gapNs_.load(std::memory_order_relaxed) < kSpinWindowNs;
        if (!quick || !spinForBatch()) {
            // Counted before waiting: run() reads the count after publishing its batch, so either
            // it sees this worker and wakes it, or this worker went to sleep after the batch was
            // out and sleeps through it (the caller takes its jobs). No batch waits for it.
            sleeping_.fetch_add(1, std::memory_order_seq_cst);
            wake_.wait();
        }
        if (stopping_.load(std::memory_order_relaxed))
            return;
        stolen_.fetch_add(work(), std::memory_order_relaxed);
    }
}

uint32_t RealtimePool::work()
{
    uint32_t ran = 0;
    uint64_t ticket = ticket_.load(std::memory_order_acquire);
    for (;;) {
        const uint64_t next = ticket & kMask;
        const uint64_t count = (ticket >> kIndexBits) & kMask;
        if (next >= count)
            return ran;
        // Claiming bumps only the index; a ticket from another batch fails the exchange
        if (!ticket_.compare_exchange_weak(ticket, ticket + 1, std::memory_order_acq_rel, std::memory_order_acquire))
            continue;
        fn_(context_, static_cast<uint32_t>(next));
        done_.fetch_add(1, std::memory_order_release);
        ++ran;
        ticket = ticket_.load(std::memory_order_acquire);
    }
}

void RealtimePool::run(uint32_t count, JobFn fn, void *context)
{
    if (count == 0)
        return;
    const int64_t now = nowNs();
    gapNs_.store(lastRunNs_ ? now - lastRunNs_ : INT64_MAX, std::memory_order_relaxed);
    lastRunNs_ = now;
    const uint32_t helpers = std::min(active(), count - 1);
    if (helpers == 0 || count > kMask) {
        for (uint32_t i = 0; i < count; ++i)
            fn(context, i);
        return;
    }
    fn_ = fn;
    context_ = context;
    done_.store(0, std::memory_order_relaxed);
    ++batch_;
    ticket_.store((batch_ << (kIndexBits + kCountBits)) | (static_cast<uint64_t>(count) << kIndexBits),
                  std::memory_order_seq_cst);
    // Wake only workers that are asleep, taking them off the count; spinning ones see the ticket
    uint32_t asleep = sleeping_.load(std::memory_order_seq_cst);
    uint32_t wake = 0;
    do {
        wake = std::min(asleep, helpers);
    } while (wake > 0 && !sleeping_.compare_exchange_weak(asleep, asleep - wake, std::memory_order_seq_cst));
    wake_.release(wake);
    work();
    // Only jobs a worker has already started are left: wait for them
    while (done_.load(std::memory_order_acquire) < count)
        pause();
    batches_.fetch_add(1, std::memory_order_relaxed);
}

}
