#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <typeindex>
#include <unordered_map>
#include <utility>
#include <vector>

#include "PostQueue.h"

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
// event. Each event type's handlers are a shared list that publish holds while it delivers, and
// that subscribe and unsubscribe copy only when a publish is holding it (copy on write), so a
// publish costs no copy of the handlers.
class EventBus {
public:
    template <typename Event>
    SubscriptionId subscribe(std::function<void(const Event &)> handler)
    {
        const SubscriptionId id = nextId_++;
        writable(handlers_[std::type_index(typeid(Event))])
            .push_back({id, [handler = std::move(handler)](const void *event) { handler(*static_cast<const Event *>(event)); }});
        return id;
    }

    void unsubscribe(SubscriptionId id)
    {
        for (auto &[type, list] : handlers_) {
            if (!list)
                continue;
            for (size_t i = 0; i < list->size(); ++i) {
                if ((*list)[i].id == id) {
                    auto &handlers = writable(list);
                    handlers.erase(handlers.begin() + static_cast<std::ptrdiff_t>(i));
                    return;
                }
            }
        }
    }

    template <typename Event>
    void publish(const Event &event)
    {
        auto found = handlers_.find(std::type_index(typeid(Event)));
        if (found == handlers_.end() || !found->second)
            return;
        // Held, not copied: a handler that subscribes or unsubscribes changes a copy of its own
        const std::shared_ptr<const std::vector<Handler>> handlers = found->second;
        for (const Handler &h : *handlers)
            h.call(&event);
    }

    template <typename Event>
    void post(Event event)
    {
        posted_.push([this, event = std::move(event)] { publish(event); });
    }

    // Called after every post (from the posting thread), so a sleeping loop wakes up. Set by the
    // Environment; must be thread-safe. Null removes it.
    void setWakeHook(std::function<void()> hook) { posted_.setWakeHook(std::move(hook)); }

    // Delivers posted events; the Environment calls this on the main thread
    void dispatch() { posted_.drain(); }

private:
    struct Handler {
        SubscriptionId id;
        std::function<void(const void *)> call;
    };
    using List = std::shared_ptr<std::vector<Handler>>;

    // The list, to change: copied first if a publish is holding it
    static std::vector<Handler> &writable(List &list)
    {
        if (!list)
            list = std::make_shared<std::vector<Handler>>();
        else if (list.use_count() > 1)
            list = std::make_shared<std::vector<Handler>>(*list);
        return *list;
    }

    std::unordered_map<std::type_index, List> handlers_;
    SubscriptionId nextId_ = 1;
    PostQueue posted_;
};

}
