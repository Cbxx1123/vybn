#pragma once

#include <algorithm>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "Daemon.h"

namespace env {

class Environment;

// Seconds on a steady clock, for idle times
double resourceClock();
// The memory the process has committed for itself (private bytes), or 0 where unknown
uint64_t processMemory();

// One kind of thing a part of the program keeps loaded (instruments, sound files...). Pools
// register with the ResourceManager, which asks each what it holds and tells it when to let go
// of what nothing uses. Main thread only.
class ResourcePool {
public:
    struct Usage {
        uint64_t bytes = 0; // everything it holds
        uint32_t items = 0;
        uint64_t idleBytes = 0; // what nothing uses now
        uint32_t idleItems = 0;
    };

    virtual ~ResourcePool() = default;
    virtual const std::string &poolName() const = 0;
    virtual Usage usage() const = 0;
    // Lets go of items nothing has used for at least `idleSeconds` (`now` on resourceClock),
    // oldest first, until `wanted` bytes are let go (0 for no limit). Returns the bytes let go.
    virtual uint64_t release(double now, double idleSeconds, uint64_t wanted) = 0;
};

// Shared items by key that the ResourceManager sweeps. An item is in use while anything besides
// the cache holds it (a track's sampler, a clip player, a preview); once nothing has for the
// manager's idle time, the cache lets go of it and its memory is freed. Asking for it again
// (find) is a use too. Main thread only.
template <typename T>
class ResourceCache : public ResourcePool {
public:
    using Size = std::function<uint64_t(const T &)>;

    ResourceCache(std::string name, Size size) : name_(std::move(name)), size_(std::move(size)) {}

    // The item, or null; finding it counts as using it
    std::shared_ptr<T> find(const std::string &key)
    {
        auto it = entries_.find(key);
        if (it == entries_.end())
            return nullptr;
        it->second.lastUsed = resourceClock();
        return it->second.item;
    }
    bool contains(const std::string &key) const { return entries_.count(key) > 0; }
    void put(const std::string &key, std::shared_ptr<T> item)
    {
        if (!item) {
            entries_.erase(key);
            return;
        }
        const uint64_t bytes = size_ ? size_(*item) : 0;
        entries_[key] = Entry{std::move(item), bytes, resourceClock()};
    }
    void erase(const std::string &key) { entries_.erase(key); }
    // Keeps only the keys `keep` says yes to
    void keepIf(const std::function<bool(const std::string &)> &keep)
    {
        for (auto it = entries_.begin(); it != entries_.end();) {
            if (keep(it->first))
                ++it;
            else
                it = entries_.erase(it);
        }
    }
    size_t size() const { return entries_.size(); }

    const std::string &poolName() const override { return name_; }

    Usage usage() const override
    {
        Usage u;
        for (const auto &[key, e] : entries_) {
            u.bytes += e.bytes;
            ++u.items;
            if (e.item.use_count() == 1) {
                u.idleBytes += e.bytes;
                ++u.idleItems;
            }
        }
        return u;
    }

    uint64_t release(double now, double idleSeconds, uint64_t wanted) override
    {
        // What is held elsewhere is in use now; the rest has been idle since it was last used
        std::vector<typename std::map<std::string, Entry>::iterator> idle;
        for (auto it = entries_.begin(); it != entries_.end(); ++it) {
            if (it->second.item.use_count() > 1)
                it->second.lastUsed = now;
            else if (now - it->second.lastUsed >= idleSeconds)
                idle.push_back(it);
        }
        std::sort(idle.begin(), idle.end(), [](const auto &a, const auto &b) { return a->second.lastUsed < b->second.lastUsed; });
        uint64_t freed = 0;
        for (auto it : idle) {
            if (wanted > 0 && freed >= wanted)
                break;
            freed += it->second.bytes;
            entries_.erase(it); // the last reference: its memory goes now
        }
        return freed;
    }

private:
    struct Entry {
        std::shared_ptr<T> item;
        uint64_t bytes = 0;
        double lastUsed = 0.0;
    };

    std::string name_;
    Size size_;
    std::map<std::string, Entry> entries_;
};

struct ResourceConfig {
    // How long something nothing uses stays loaded, so a track switched back or an undone
    // delete doesn't reload it from disk
    double idleSeconds = 30.0;
    // Past this, idle things go at once, oldest first, whatever their idle time
    uint64_t budgetBytes = 512ull << 20;
};

// The program's loaded resources in one place: every pool of things kept in memory (the
// instruments, the sound files) registers here, and once a second it lets go of what nothing has
// used for the idle time, sooner when the pools together are over the budget. It also reports
// what each pool holds. A daemon: add it before whatever registers pools. Main thread only.
class ResourceManager : public Daemon {
public:
    explicit ResourceManager(Environment &env, ResourceConfig config = {});

    void add(ResourcePool &pool);
    void remove(ResourcePool &pool);

    void configure(const ResourceConfig &config) { config_ = config; }
    const ResourceConfig &config() const { return config_; }

    // Lets go of everything nothing uses now, whatever its idle time; returns the bytes freed
    uint64_t trim();
    // Runs a sweep now, as tick does once a second
    uint64_t sweep(double now);

    struct PoolStats {
        std::string name;
        ResourcePool::Usage usage;
        uint64_t freed = 0; // let go of since start
    };
    std::vector<PoolStats> stats() const;
    uint64_t freedBytes() const { return freed_; }

    void tick(Environment &env) override;

private:
    struct Entry {
        ResourcePool *pool;
        uint64_t freed;
    };

    ResourceConfig config_;
    std::vector<Entry> pools_;
    uint64_t freed_ = 0;
    double nextSweep_ = 0.0;
};

}
