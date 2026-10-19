#include "Environment.h"

#include <algorithm>
#include <chrono>

namespace env {

Environment::~Environment()
{
    // Reverse order: a daemon goes before the daemons it depends on
    while (!daemons_.empty())
        daemons_.pop_back();
}

void Environment::post(std::function<void()> action)
{
    std::lock_guard<std::mutex> lock(postedMutex_);
    posted_.push_back(std::move(action));
    if (wake_)
        wake_();
}

void Environment::setWakeHook(std::function<void()> hook)
{
    std::lock_guard<std::mutex> lock(postedMutex_);
    wake_ = hook;
    events_.setWakeHook(std::move(hook));
}

void Environment::runPosted()
{
    std::vector<std::function<void()>> actions;
    {
        std::lock_guard<std::mutex> lock(postedMutex_);
        actions.swap(posted_);
    }
    for (auto &action : actions)
        action();
}

void Environment::run()
{
    using Clock = std::chrono::steady_clock;

    stopRequested_ = false;
    running_ = true;
    for (auto &d : daemons_)
        d->start(*this);

    while (!stopRequested_) {
        runPosted();
        events_.dispatch();
        for (size_t i = 0; i < daemons_.size(); ++i) {
            Daemon &d = *daemons_[i];
            const auto begin = Clock::now();
            d.tick(*this);
            const double total = std::chrono::duration<double, std::milli>(Clock::now() - begin).count();
            // Time spent blocked (waiting for vsync, say) isn't work
            const double waited = std::min(total, d.takeWaited());
            const double ms = total - waited;

            // At 60 ticks a second these settle over roughly a second. The first few ticks
            // include one-off start-up work, so they set the values rather than blend in.
            DaemonStats &s = stats_[i];
            const bool warmingUp = frame_ < 3;
            s.lastMs = ms;
            s.averageMs = warmingUp ? ms : s.averageMs + (ms - s.averageMs) * 0.05;
            s.waitMs = warmingUp ? waited : s.waitMs + (waited - s.waitMs) * 0.05;
            s.peakMs = warmingUp ? ms : std::max(ms, s.peakMs * 0.98);
        }
        ++frame_;
    }

    for (size_t i = daemons_.size(); i-- > 0;)
        daemons_[i]->stop(*this);
    running_ = false;
}

}
