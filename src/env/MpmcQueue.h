#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <utility>

namespace env {

// A fixed-size, lock-free queue for any number of producer and consumer threads (Dmitry
// Vyukov's bounded queue). push and pop never block or allocate, so real-time threads may use
// it: several audio workers can push requests that background threads pop. Holds Capacity items.
template <typename T, size_t Capacity>
class MpmcQueue {
    static_assert(Capacity >= 2 && (Capacity & (Capacity - 1)) == 0, "Capacity must be a power of two");

public:
    MpmcQueue()
    {
        for (size_t i = 0; i < Capacity; ++i)
            cells_[i].sequence.store(i, std::memory_order_relaxed);
    }

    // Returns false, dropping the item, when the queue is full
    bool push(T item)
    {
        size_t position = head_.load(std::memory_order_relaxed);
        for (;;) {
            Cell &cell = cells_[position & (Capacity - 1)];
            const size_t sequence = cell.sequence.load(std::memory_order_acquire);
            const auto diff = static_cast<std::ptrdiff_t>(sequence) - static_cast<std::ptrdiff_t>(position);
            if (diff == 0) {
                if (head_.compare_exchange_weak(position, position + 1, std::memory_order_relaxed)) {
                    cell.item = std::move(item);
                    cell.sequence.store(position + 1, std::memory_order_release);
                    return true;
                }
            } else if (diff < 0) {
                return false; // full
            } else {
                position = head_.load(std::memory_order_relaxed);
            }
        }
    }

    // Returns false when the queue is empty
    bool pop(T &out)
    {
        size_t position = tail_.load(std::memory_order_relaxed);
        for (;;) {
            Cell &cell = cells_[position & (Capacity - 1)];
            const size_t sequence = cell.sequence.load(std::memory_order_acquire);
            const auto diff = static_cast<std::ptrdiff_t>(sequence) - static_cast<std::ptrdiff_t>(position + 1);
            if (diff == 0) {
                if (tail_.compare_exchange_weak(position, position + 1, std::memory_order_relaxed)) {
                    out = std::move(cell.item);
                    cell.sequence.store(position + Capacity, std::memory_order_release);
                    return true;
                }
            } else if (diff < 0) {
                return false; // empty
            } else {
                position = tail_.load(std::memory_order_relaxed);
            }
        }
    }

private:
    struct Cell {
        std::atomic<size_t> sequence;
        T item;
    };
    std::array<Cell, Capacity> cells_;
    alignas(64) std::atomic<size_t> head_{0};
    alignas(64) std::atomic<size_t> tail_{0};
};

}
