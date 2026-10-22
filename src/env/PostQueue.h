#pragma once

#include <functional>
#include <mutex>
#include <utility>
#include <vector>

namespace env {

// Work handed to the main thread from any thread: push queues it under a lock and calls the wake
// hook (so a loop sleeping in its event wait wakes at once); drain runs what was queued, in order,
// on the calling thread. What is pushed while a drain runs waits for the next one. The two
// buffers swap rather than being replaced, so a steady stream of posts allocates nothing once
// they have grown. The lock makes push unfit for real-time threads (they use an SpscQueue).
// The Environment's posted actions and the EventBus's posted events are one each.
class PostQueue {
public:
    void push(std::function<void()> work)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        queued_.push_back(std::move(work));
        if (wake_)
            wake_();
    }

    // Called after every push, from the pushing thread; must be thread-safe. Null removes it.
    void setWakeHook(std::function<void()> hook)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        wake_ = std::move(hook);
    }

    // Main thread: runs everything queued so far; returns whether there was anything
    bool drain()
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            running_.swap(queued_); // running_ is empty here, with the capacity it grew to
        }
        for (auto &work : running_)
            work();
        const bool any = !running_.empty();
        running_.clear();
        return any;
    }

private:
    std::mutex mutex_; // guards queued_ and wake_
    std::vector<std::function<void()>> queued_;
    std::vector<std::function<void()>> running_; // drain's, on the main thread
    std::function<void()> wake_;
};

}
