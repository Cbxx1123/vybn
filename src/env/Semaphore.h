#pragma once

#include <cstdint>

namespace env {

// A counting semaphore. release() never blocks and takes no lock (on Windows it is one kernel
// call), so a real-time thread may use it to wake a sleeping worker. wait() blocks until a
// release, or until the timeout passes (returning false).
class Semaphore {
public:
    Semaphore();
    ~Semaphore();
    Semaphore(const Semaphore &) = delete;
    Semaphore &operator=(const Semaphore &) = delete;

    void release(uint32_t count = 1);
    void wait();
    bool waitFor(uint32_t milliseconds);

private:
    void *handle_ = nullptr; // the platform's semaphore (or a fallback's state)
};

}
