#include "Resources.h"

#include <algorithm>
#include <chrono>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define PSAPI_VERSION 2
#include <windows.h>
#include <psapi.h>
#endif

namespace env {

double resourceClock()
{
    using namespace std::chrono;
    static const steady_clock::time_point start = steady_clock::now();
    return duration<double>(steady_clock::now() - start).count();
}

uint64_t processMemory()
{
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS_EX counters{};
    counters.cb = sizeof counters;
    if (K32GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS *>(&counters), sizeof counters))
        return counters.PrivateUsage;
#endif
    return 0;
}

ResourceManager::ResourceManager(Environment &, ResourceConfig config) : Daemon("Resources"), config_(config) {}

void ResourceManager::add(ResourcePool &pool)
{
    pools_.push_back({&pool, 0});
}

void ResourceManager::remove(ResourcePool &pool)
{
    pools_.erase(std::remove_if(pools_.begin(), pools_.end(), [&](const Entry &e) { return e.pool == &pool; }), pools_.end());
}

uint64_t ResourceManager::sweep(double now)
{
    uint64_t freed = 0;
    // What has been idle long enough
    for (Entry &e : pools_) {
        const uint64_t bytes = e.pool->release(now, config_.idleSeconds, 0);
        e.freed += bytes;
        freed += bytes;
    }
    // Over the budget: idle things go now, from each pool in turn, until it fits
    uint64_t total = 0;
    for (const Entry &e : pools_)
        total += e.pool->usage().bytes;
    for (Entry &e : pools_) {
        if (total <= config_.budgetBytes)
            break;
        const uint64_t bytes = e.pool->release(now, 0.0, total - config_.budgetBytes);
        e.freed += bytes;
        freed += bytes;
        total -= std::min(total, bytes);
    }
    freed_ += freed;
    return freed;
}

uint64_t ResourceManager::trim()
{
    const double now = resourceClock();
    uint64_t freed = 0;
    for (Entry &e : pools_) {
        const uint64_t bytes = e.pool->release(now, 0.0, 0);
        e.freed += bytes;
        freed += bytes;
    }
    freed_ += freed;
    return freed;
}

std::vector<ResourceManager::PoolStats> ResourceManager::stats() const
{
    std::vector<PoolStats> out;
    for (const Entry &e : pools_)
        out.push_back({e.pool->poolName(), e.pool->usage(), e.freed});
    return out;
}

void ResourceManager::tick(Environment &)
{
    const double now = resourceClock();
    if (now < nextSweep_)
        return;
    nextSweep_ = now + 1.0;
    sweep(now);
}

}
