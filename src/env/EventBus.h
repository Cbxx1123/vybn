#pragma once

#include <cstdint>
#include <functional>
#include <mutex>
#include <typeindex>
#include <unordered_map>
#include <utility>
#include <vector>

namespace env {

using SubscriptionId = uint64_t;

// Typed messages between daemons. Any copyable struct can be an event.
// - publish delivers to every subscriber right away, on the calling thread. Use it on the main
//   thread, for notices that must be handled before the caller goes on (a window about to be
//   destroyed, say).
// - post can be called from any thread; the event is delivered on the main thread the next time
//   the Environment dispatches (at the start of each loop iteration). It takes a lock, so it is
//   not for real-time threads such as an audio callback.
// Subscribing or unsubscribing from inside a handler is allowed; it takes effect for the next
// event.
class EventBus {
public:
    template <typename Event>
    SubscriptionId subscribe(std::function<void(const Event &)> handler)
    {
        const SubscriptionId id = nextId_++;
        handlers_[std::type_index(typeid(Event))].push_back(
            {id, [handler = std::move(handler)](const void *event) { handler(*static_cast<const Event *>(event)); }});
        return id;
    }

    void unsubscribe(SubscriptionId id)
    {
        for (auto &[type, list] : handlers_) {
            for (auto it = list.begin(); it != list.end(); ++it) {
                if (it->id == id) {
                    list.erase(it);
                    return;
                }
            }
        }
    }

    template <typename Event>
    void publish(const Event &event)
    {
        auto found = handlers_.find(std::type_index(typeid(Event)));
        if (found == handlers_.end())
            return;
        // A copy, so handlers can subscribe and unsubscribe while being called
        const std::vector<Handler> handlers = found->second;
        for (const Handler &h : handlers)
            h.call(&event);
    }

    template <typename Event>
    void post(Event event)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        queued_.push_back([this, event = std::move(event)] { publish(event); });
        if (wake_)
            wake_();
    }

    // Called after every post (from the posting thread), so a sleeping loop wakes up. Set by the
    // Environment; must be thread-safe. Null removes it.
    void setWakeHook(std::function<void()> hook)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        wake_ = std::move(hook);
    }

    // Delivers posted events; the Environment calls this on the main thread
    void dispatch()
    {
        std::vector<std::function<void()>> events;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            events.swap(queued_);
        }
        for (auto &deliver : events)
            deliver();
    }

private:
    struct Handler {
        SubscriptionId id;
        std::function<void(const void *)> call;
    };

    std::unordered_map<std::type_index, std::vector<Handler>> handlers_;
    SubscriptionId nextId_ = 1;
    std::mutex mutex_;
    std::vector<std::function<void()>> queued_;
    std::function<void()> wake_;
};

}
