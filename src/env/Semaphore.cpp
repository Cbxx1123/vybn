#include "Semaphore.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <chrono>
#include <condition_variable>
#include <mutex>
#endif

namespace env {

#ifdef _WIN32

Semaphore::Semaphore() : handle_(CreateSemaphoreW(nullptr, 0, 0x7fffffff, nullptr)) {}

Semaphore::~Semaphore()
{
    if (handle_)
        CloseHandle(static_cast<HANDLE>(handle_));
}

void Semaphore::release(uint32_t count)
{
    if (count > 0)
        ReleaseSemaphore(static_cast<HANDLE>(handle_), static_cast<LONG>(count), nullptr);
}

void Semaphore::wait()
{
    WaitForSingleObject(static_cast<HANDLE>(handle_), INFINITE);
}

bool Semaphore::waitFor(uint32_t milliseconds)
{
    return WaitForSingleObject(static_cast<HANDLE>(handle_), milliseconds) == WAIT_OBJECT_0;
}

#else

namespace {
struct State {
    std::mutex mutex;
    std::condition_variable ready;
    uint32_t count = 0;
};
}

Semaphore::Semaphore() : handle_(new State) {}

Semaphore::~Semaphore()
{
    delete static_cast<State *>(handle_);
}

void Semaphore::release(uint32_t count)
{
    auto &s = *static_cast<State *>(handle_);
    {
        std::lock_guard<std::mutex> lock(s.mutex);
        s.count += count;
    }
    s.ready.notify_all();
}

void Semaphore::wait()
{
    auto &s = *static_cast<State *>(handle_);
    std::unique_lock<std::mutex> lock(s.mutex);
    s.ready.wait(lock, [&s] { return s.count > 0; });
    --s.count;
}

bool Semaphore::waitFor(uint32_t milliseconds)
{
    auto &s = *static_cast<State *>(handle_);
    std::unique_lock<std::mutex> lock(s.mutex);
    if (!s.ready.wait_for(lock, std::chrono::milliseconds(milliseconds), [&s] { return s.count > 0; }))
        return false;
    --s.count;
    return true;
}

#endif

}
