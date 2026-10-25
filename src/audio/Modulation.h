#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "Automation.h"
#include "Device.h"
#include "Effects.h"

namespace audio {

// Modulators (docs/design/modulators-plan.md): sources that move parameters as the song plays. A
// track keeps its modulators (each a DeviceSetting of kind Modulator, with an id unique in the track),
// and what they move keeps the routes (DeviceSetting::modulation, and the track's own for its volume
// and pan), so a route goes wherever its device goes.

enum class ModulatorKind { Lfo, Steps, Follower, Macro };
constexpr int kModulatorKindCount = 4;
constexpr size_t kMaxModulators = 8; // on one track
constexpr size_t kMaxModulatorParams = 19;

struct ModulatorInfo {
    const char *name;
    const char *key; // the device id in project files
    size_t paramCount;
    std::array<ParamInfo, kMaxModulatorParams> params;
};
const ModulatorInfo &modulatorInfo(ModulatorKind kind);
bool modulatorFromKey(const std::string &key, ModulatorKind &kind);
// A modulator's setting at its defaults (DeviceKind::Modulator)
DeviceSetting modulatorSetting(ModulatorKind kind);
// Its parameters as device parameters
std::vector<DeviceParam> modulatorParams(ModulatorKind kind);
// Whether its values swing both ways (an LFO set to bipolar: -1 to +1), else 0 to 1
bool modulatorBipolar(const DeviceSetting &setting);

// A modulator in a track
struct Modulator {
    uint32_t id = 0; // unique in the track, never 0
    DeviceSetting setting;

    bool operator==(const Modulator &o) const { return id == o.id && setting == o.setting; }
    bool operator!=(const Modulator &o) const { return !(*this == o); }
};

// The synced lengths (LFO and Steps rates) in beats
double syncedBeats(int choice);

// Where a block is for modulators: the song's beat and seconds while it plays, else free-running ones
struct ModClock {
    double beat = 0.0;
    double seconds = 0.0;
    double tempo = 120.0;
};

// A modulator on the audio thread: its parameters (atomics, set from the main thread) and its state.
// value() is its output at a moment; follow() feeds the follower a block's level.
class ModulatorState {
public:
    explicit ModulatorState(ModulatorKind kind);

    ModulatorKind kind() const { return kind_; }
    // Main thread: takes the setting's values and bypass
    void apply(const DeviceSetting &setting);
    // Audio thread: the output now (bipolar or not as set), 0 while bypassed
    float value(const ModClock &clock) const;
    // Audio thread: a block's peak level of what the track played, lasting `seconds`
    void follow(float level, double seconds);
    // The last value worked out (for the interface; any thread)
    float shown() const { return shown_.load(std::memory_order_relaxed); }
    void show(float v) { shown_.store(v, std::memory_order_relaxed); }

private:
    float param(size_t i) const { return values_[i].load(std::memory_order_relaxed); }

    ModulatorKind kind_;
    std::array<std::atomic<float>, kMaxModulatorParams> values_{};
    std::atomic<bool> bypass_{false};
    std::atomic<float> shown_{0.0f};
    float envelope_ = 0.0f; // the follower's (audio thread)
};

// A route made ready for the audio thread: the modulator's slot, the parameter, its curve (knob position
// to value), its value when nothing automates it, and the depth
struct ResolvedRoute {
    size_t slot = 0;
    uint32_t param = 0;
    ParamCurve curve;
    double base = 0.0;
    float depth = 0.0f;
};
using ResolvedRoutes = std::vector<ResolvedRoute>;

// A parameter's value with the modulators' routes to it added: its knob position plus each route's
// depth times its modulator's value, clamped, along its curve. `values` are the modulators' this block.
double modulatedValue(double value, const ParamCurve &curve, const ResolvedRoute *routes, size_t count, const float *values);

}
