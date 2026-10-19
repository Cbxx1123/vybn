#pragma once

#include <algorithm>
#include <cstdint>
#include <functional>
#include <memory>
#include <utility>
#include <vector>

namespace env {

// A hook on a Signal. Destroying it (or calling disconnect) removes the hook, so a component
// that keeps its Connections as members is unhooked when it is destroyed. It may outlive the
// signal. Move-only.
class Connection {
public:
    Connection() = default;
    explicit Connection(std::function<void()> disconnect) : disconnect_(std::move(disconnect)) {}
    Connection(Connection &&other) noexcept : disconnect_(std::move(other.disconnect_)) { other.disconnect_ = nullptr; }
    Connection &operator=(Connection &&other) noexcept
    {
        if (this != &other) {
            disconnect();
            disconnect_ = std::move(other.disconnect_);
            other.disconnect_ = nullptr;
        }
        return *this;
    }
    Connection(const Connection &) = delete;
    Connection &operator=(const Connection &) = delete;
    ~Connection() { disconnect(); }

    void disconnect()
    {
        if (disconnect_) {
            auto d = std::move(disconnect_);
            disconnect_ = nullptr;
            d();
        }
    }
    bool connected() const { return static_cast<bool>(disconnect_); }

private:
    std::function<void()> disconnect_;
};

// A list of hooks that an event is offered to, highest priority first and, among equal
// priorities, the most recently connected first (so the newest dialog or menu sees keys before
// the ones under it). A handler returns true when it has dealt with the event, which stops it
// there; emit returns whether anyone did. Handlers may connect and disconnect hooks, including
// their own, while being called: hooks added during an emit wait for the next one.
// Main thread only: it doesn't lock.
template <typename... Args>
class Signal {
public:
    using Handler = std::function<bool(Args...)>;

    Signal() : state_(std::make_shared<State>()) {}
    Signal(const Signal &) = delete;
    Signal &operator=(const Signal &) = delete;

    [[nodiscard]] Connection connect(Handler handler, int priority = 0)
    {
        auto slot = std::make_shared<Slot>();
        slot->handler = std::move(handler);
        slot->priority = priority;
        slot->order = state_->next++;
        // Keep the list sorted: priority high to low, then newest first
        auto &slots = state_->slots;
        auto at = std::find_if(slots.begin(), slots.end(), [&](const std::shared_ptr<Slot> &s) {
            return s->priority < slot->priority || (s->priority == slot->priority && s->order < slot->order);
        });
        slots.insert(at, slot);
        std::weak_ptr<State> weakState = state_;
        std::weak_ptr<Slot> weakSlot = slot;
        return Connection([weakState, weakSlot] {
            auto state = weakState.lock();
            auto slot = weakSlot.lock();
            if (!state || !slot)
                return;
            slot->alive = false;
            auto &list = state->slots;
            list.erase(std::remove(list.begin(), list.end(), slot), list.end());
        });
    }

    bool emit(Args... args) const
    {
        // A copy, so handlers can change the list
        const std::vector<std::shared_ptr<Slot>> slots = state_->slots;
        for (const auto &slot : slots) {
            if (slot->alive && slot->handler(args...))
                return true;
        }
        return false;
    }

    size_t size() const { return state_->slots.size(); }

private:
    struct Slot {
        Handler handler;
        int priority = 0;
        uint64_t order = 0;
        bool alive = true;
    };
    struct State {
        std::vector<std::shared_ptr<Slot>> slots;
        uint64_t next = 0;
    };
    std::shared_ptr<State> state_;
};

}
