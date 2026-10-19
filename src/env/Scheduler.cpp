#include "Scheduler.h"

#include <algorithm>
#include <utility>

#include "Environment.h"

#ifdef _WIN32
#include <windows.h>
#endif

namespace env {

const char *laneName(Lane lane)
{
    switch (lane) {
    case Lane::Interactive: return "Interactive";
    case Lane::Background: return "Background";
    case Lane::Disk: return "Disk";
    case Lane::Network: return "Network";
    }
    return "?";
}

void runJob(Environment &env, Lane lane, std::function<void()> job, JobGroup &group)
{
    if (Scheduler *scheduler = env.find<Scheduler>()) {
        scheduler->submit(lane, std::move(job), &group);
        return;
    }
    group.add();
    std::thread([job = std::move(job), &group] {
        job();
        group.finish();
    }).detach();
}

void JobGroup::finish()
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (--pending_ == 0)
        idle_.notify_all();
}

void JobGroup::wait()
{
    std::unique_lock<std::mutex> lock(mutex_);
    idle_.wait(lock, [this] { return pending_.load() == 0; });
}

uint32_t Scheduler::cores()
{
    const unsigned n = std::thread::hardware_concurrency();
    return n == 0 ? 2u : n;
}

Scheduler::Scheduler(Environment &, SchedulerConfig config) : Daemon("Scheduler")
{
    const uint32_t n = cores();
    // The audio thread is one core; the pool takes the rest, up to 15
    const uint32_t realtime = config.realtimeWorkers == 0xffffffff ? std::min<uint32_t>(n - 1, 15) : config.realtimeWorkers;
    realtime_.start(realtime);
    startPool(workers_, config.workers ? config.workers : std::max<uint32_t>(2, n / 2), {Lane::Interactive, Lane::Background},
              "worker");
    startPool(disk_, std::max<uint32_t>(1, config.diskThreads), {Lane::Disk}, "disk");
    startPool(network_, std::max<uint32_t>(1, config.networkThreads), {Lane::Network}, "network");
}

Scheduler::~Scheduler()
{
    shutdown();
}

Scheduler::Pool &Scheduler::poolFor(Lane lane)
{
    return lane == Lane::Disk ? disk_ : lane == Lane::Network ? network_ : workers_;
}

const Scheduler::Pool &Scheduler::poolFor(Lane lane) const
{
    return lane == Lane::Disk ? disk_ : lane == Lane::Network ? network_ : workers_;
}

void Scheduler::startPool(Pool &pool, uint32_t threads, std::vector<Lane> lanes, const char *)
{
    pool.lanes = std::move(lanes);
    for (uint32_t i = 0; i < threads; ++i)
        pool.threads.emplace_back([this, &pool] { loop(pool); });
}

void Scheduler::submit(Lane lane, std::function<void()> job, JobGroup *group)
{
    if (group)
        group->add();
    if (stopping_) {
        if (group)
            group->finish();
        return;
    }
    Pool &pool = poolFor(lane);
    {
        std::lock_guard<std::mutex> lock(pool.mutex);
        pool.queues[static_cast<size_t>(lane)].push_back({std::move(job), group});
    }
    pool.wake.release();
}

void Scheduler::wake(Lane lane)
{
    poolFor(lane).wake.release();
}

int Scheduler::addPoller(Lane lane, std::function<bool()> fn, uint32_t intervalMs)
{
    Pool &pool = poolFor(lane);
    auto poller = std::make_shared<Poller>();
    poller->id = nextPoller_++;
    poller->fn = std::move(fn);
    poller->intervalMs = std::max<uint32_t>(1, intervalMs);
    {
        std::lock_guard<std::mutex> lock(pool.mutex);
        pool.pollers.push_back(poller);
    }
    pool.wake.release(static_cast<uint32_t>(pool.threads.size()));
    return poller->id;
}

void Scheduler::removePoller(int id)
{
    for (Pool *pool : {&workers_, &disk_, &network_}) {
        std::shared_ptr<Poller> removed;
        {
            std::lock_guard<std::mutex> lock(pool->mutex);
            for (auto it = pool->pollers.begin(); it != pool->pollers.end(); ++it) {
                if ((*it)->id == id) {
                    removed = *it;
                    pool->pollers.erase(it);
                    break;
                }
            }
        }
        // Calls already under way finish before it is gone
        while (removed && removed->running.load() > 0)
            std::this_thread::yield();
    }
}

bool Scheduler::runPollers(Pool &pool)
{
    std::vector<std::shared_ptr<Poller>> pollers;
    {
        std::lock_guard<std::mutex> lock(pool.mutex);
        pollers = pool.pollers;
    }
    bool any = false;
    for (auto &p : pollers) {
        ++p->running;
        // Removed between the copy and now: skip it
        bool listed;
        {
            std::lock_guard<std::mutex> lock(pool.mutex);
            listed = std::find(pool.pollers.begin(), pool.pollers.end(), p) != pool.pollers.end();
        }
        if (listed)
            any = p->fn() || any;
        --p->running;
    }
    return any;
}

void Scheduler::loop(Pool &pool)
{
#ifdef _WIN32
    // Background threads give way to the UI and audio
    if (&pool != &disk_)
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    else
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL); // reads feed the audio
#endif
    for (;;) {
        uint32_t interval = 1000;
        {
            std::lock_guard<std::mutex> lock(pool.mutex);
            for (const auto &p : pool.pollers)
                interval = std::min(interval, p->intervalMs);
        }
        pool.wake.waitFor(interval);
        if (stopping_)
            return;
        // Jobs, the first lane's first
        for (;;) {
            Job job;
            Lane lane = Lane::Background;
            {
                std::lock_guard<std::mutex> lock(pool.mutex);
                bool found = false;
                for (Lane l : pool.lanes) {
                    auto &queue = pool.queues[static_cast<size_t>(l)];
                    if (!queue.empty()) {
                        job = std::move(queue.front());
                        queue.pop_front();
                        lane = l;
                        found = true;
                        break;
                    }
                }
                if (!found)
                    break;
            }
            ++pool.busy;
            job.fn();
            job.fn = nullptr; // what it holds goes before the group says it's done
            --pool.busy;
            ++pool.completed[static_cast<size_t>(lane)];
            if (job.group)
                job.group->finish();
            if (stopping_)
                return;
        }
        while (!stopping_ && runPollers(pool)) {
        }
    }
}

Scheduler::LaneStats Scheduler::stats(Lane lane) const
{
    const Pool &pool = poolFor(lane);
    LaneStats s;
    s.threads = static_cast<uint32_t>(pool.threads.size());
    s.busy = pool.busy.load();
    s.completed = pool.completed[static_cast<size_t>(lane)].load();
    std::lock_guard<std::mutex> lock(pool.mutex);
    s.queued = pool.queues[static_cast<size_t>(lane)].size();
    return s;
}

void Scheduler::stop(Environment &)
{
    shutdown();
}

void Scheduler::shutdown()
{
    if (stopping_.exchange(true))
        return;
    for (auto &hook : stopHooks_)
        hook();
    realtime_.stop();
    for (Pool *pool : {&workers_, &disk_, &network_}) {
        pool->wake.release(static_cast<uint32_t>(pool->threads.size()));
        for (std::thread &t : pool->threads)
            t.join();
        pool->threads.clear();
        // Jobs that never started count as finished, so nobody waits for them
        for (auto &queue : pool->queues) {
            for (Job &job : queue) {
                job.fn = nullptr;
                if (job.group)
                    job.group->finish();
            }
            queue.clear();
        }
    }
}

}
