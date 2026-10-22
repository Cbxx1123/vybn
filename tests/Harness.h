#pragma once

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <utility>
#include <vector>

// The one checker every self-test suite uses, and the switches that choose how much a run does.
// See docs/testing.md.
//
// A check prints a PASS/FAIL line as it always has (`PASS  <name> <detail>`), and records how long the
// work leading up to it took (the time since the previous check), so a slow suite can be profiled by
// check instead of guessed at.
//
// Three tiers, chosen by environment variables (tools/test.sh sets them from its flags):
//   default            the fast tier: every assertion, on inputs just big enough to prove it. This is
//                      what runs while working and in the commit gate.
//   VIBIN_TEST_FULL=1  the same assertions on the full-size inputs (the whole demo song, every voice
//                      count, 160,000 notes): slower, for before a release or when changing the engine.
//   VIBIN_TEST_BENCH=1 also run the benchmarks: they measure and print INFO lines, and assert nothing.
// And two output switches:
//   VIBIN_TEST_TIMES=1 after the suite, print its slowest checks
//   VIBIN_TEST_QUIET=1 print only FAIL, SKIP and INFO lines and the summary
namespace tests {

inline bool flag(const char *name)
{
    const char *v = std::getenv(name);
    return v && *v && std::string(v) != "0";
}

// Whether to use the full-size inputs (see above). A check keeps its assertion either way; only the
// size of what it runs on changes, so `full() ? bigSize : smallSize` is the whole use.
inline bool full()
{
    static const bool on = flag("VIBIN_TEST_FULL");
    return on;
}

// Whether to run the benchmarks: code that times something and prints it, with nothing to pass or fail
inline bool bench()
{
    static const bool on = flag("VIBIN_TEST_BENCH");
    return on;
}

// Paces a loop to a fixed period, as the audio device would: step n may begin at n * period after the
// first. std::this_thread::sleep_for(2.5 ms) sleeps a whole timer tick (about 15 ms on Windows), which
// made a "twice real time" loop run at a seventh of real time; this waits to the exact moment instead
// (sleeping only while the wait is long, spinning with yield for the last stretch).
class Pacer {
public:
    explicit Pacer(std::chrono::microseconds period) : period_(period), next_(Clock::now()) {}

    void wait()
    {
        next_ += period_;
        for (;;) {
            const auto left = next_ - Clock::now();
            if (left <= Clock::duration::zero())
                return;
            if (left > std::chrono::milliseconds(3))
                std::this_thread::sleep_for(left - std::chrono::milliseconds(2));
            else
                std::this_thread::yield();
        }
    }

private:
    using Clock = std::chrono::steady_clock;
    std::chrono::microseconds period_;
    Clock::time_point next_;
};

class Checker {
public:
    int failures = 0;
    int passes = 0;
    int skips = 0;

    Checker() : start_(Clock::now()), mark_(start_) {}

    void check(bool ok, const std::string &what, const std::string &detail = {})
    {
        record(what);
        if (ok)
            ++passes;
        else
            ++failures;
        if (!ok || !quiet())
            std::printf("%s  %-54s %s\n", ok ? "PASS" : "FAIL", what.c_str(), detail.c_str());
    }

    void skip(const std::string &what, const std::string &why)
    {
        ++skips;
        std::printf("SKIP  %-54s %s\n", what.c_str(), why.c_str());
        mark_ = Clock::now(); // a skipped check did no work
    }

    // Ends the suite: the summary line, and with VIBIN_TEST_TIMES the slowest checks. Returns the exit code.
    int finish(const char *suite)
    {
        const double total = millis(start_, Clock::now());
        if (flag("VIBIN_TEST_TIMES")) {
            std::vector<std::pair<double, std::string>> sorted = costs_;
            std::sort(sorted.begin(), sorted.end(), [](const auto &a, const auto &b) { return a.first > b.first; });
            for (size_t i = 0; i < sorted.size() && i < 8; ++i)
                std::printf("TIME  %-7.0f ms  %s\n", sorted[i].first, sorted[i].second.c_str());
        }
        if (failures == 0)
            std::printf("All passed: 0 failed (%s: %d checks, %d skipped, %.0f ms)\n", suite, passes, skips, total);
        else
            std::printf("%d FAILED (%s: %d passed, %d skipped, %.0f ms)\n", failures, suite, passes, skips, total);
        return failures == 0 ? 0 : 1;
    }

private:
    using Clock = std::chrono::steady_clock;

    static double millis(Clock::time_point a, Clock::time_point b)
    {
        return std::chrono::duration<double, std::milli>(b - a).count();
    }
    static bool quiet()
    {
        static const bool on = flag("VIBIN_TEST_QUIET");
        return on;
    }
    void record(const std::string &what)
    {
        const Clock::time_point now = Clock::now();
        costs_.emplace_back(millis(mark_, now), what);
        mark_ = now;
    }

    Clock::time_point start_, mark_;
    std::vector<std::pair<double, std::string>> costs_;
};

}
