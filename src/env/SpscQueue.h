#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <utility>

namespace env {

// A fixed-size, lock-free queue for exactly one producer thread and one consumer thread, such as
// the main thread and an audio thread. push and pop never block or allocate, so they are safe on
// a real-time thread. Holds Capacity - 1 items. T must be cheap to copy or move.
template <typename T, size_t Capacity>
class SpscQueue {
    static_assert(Capacity >= 2 && (Capacity & (Capacity - 1)) == 0, "Capacity must be a power of two");

public:
    // Producer only. Returns false, dropping the item, when the queue is full.
    bool push(T item)
    {
        const size_t head = head_.load(std::memory_order_relaxed);
        const size_t next = (head + 1) & (Capacity - 1);
        if (next == tail_.load(std::memory_order_acquire))
            return false;
        items_[head] = std::move(item);
        head_.store(next, std::memory_order_release);
        return true;
    }

    // Producer only: whether a push would fail now
    bool full() const
    {
        const size_t next = (head_.load(std::memory_order_relaxed) + 1) & (Capacity - 1);
        return next == tail_.load(std::memory_order_acquire);
    }

    // Consumer only. Returns false when the queue is empty.
    bool pop(T &item)
    {
        const size_t tail = tail_.load(std::memory_order_relaxed);
        if (tail == head_.load(std::memory_order_acquire))
            return false;
        item = std::move(items_[tail]);
        tail_.store((tail + 1) & (Capacity - 1), std::memory_order_release);
        return true;
    }

    // Either side; only a hint while the other side is active
    bool empty() const { return head_.load(std::memory_order_acquire) == tail_.load(std::memory_order_acquire); }

private:
    std::array<T, Capacity> items_{};
    // On separate cache lines, so the two threads don't contend for one
    alignas(64) std::atomic<size_t> head_{0}; // written by the producer
    alignas(64) std::atomic<size_t> tail_{0}; // written by the consumer
};

}
