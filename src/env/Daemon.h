#pragma once

#include <string>
#include <utility>

namespace env {

class Environment;

// A system the Environment runs for the life of the program: windowing, UI, and later audio.
//
// Lifecycle, all on the main thread:
// - The constructor sets the daemon up. Its first argument is the Environment, through which it
//   finds the daemons it depends on (Environment::require); those must have been added first.
// - start() runs once when the Environment starts running. A daemon with a thread of its own
//   (an audio engine's real-time thread, say) starts it here.
// - tick() runs once per loop iteration, in the order the daemons were added.
// - stop() runs once when the Environment stops, in reverse order. Join threads here.
// - Daemons are destroyed in reverse order, so a daemon can rely on the ones it depends on
//   outliving it.
class Daemon {
public:
    explicit Daemon(std::string name) : name_(std::move(name)) {}
    virtual ~Daemon() = default;

    Daemon(const Daemon &) = delete;
    Daemon &operator=(const Daemon &) = delete;

    const std::string &name() const { return name_; }

    virtual void start(Environment &) {}
    virtual void tick(Environment &) {}
    virtual void stop(Environment &) {}

    // Time this daemon spent blocked since the last call (the Environment calls it after each
    // tick, to tell work from waiting)
    double takeWaited()
    {
        const double ms = waited_;
        waited_ = 0.0;
        return ms;
    }

protected:
    // Reports time spent blocked during this tick, waiting on something other than work (vsync,
    // a device, a lock), so status displays don't count it as load
    void reportWait(double ms) { waited_ += ms; }

private:
    std::string name_;
    double waited_ = 0.0;
};

}
