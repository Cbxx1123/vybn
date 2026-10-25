#include "Modulation.h"

#include <algorithm>
#include <cmath>

#include "MidiEffects.h"

namespace audio {

namespace {

using U = ParamUnit;
constexpr double kTwoPi = 6.283185307179586;

#define SYNC_CHOICES "4 bars|2 bars|1 bar|1/2|1/4|1/8|1/16|1/32|1/4T|1/8T|1/16T"
constexpr double kSynced[] = {16.0, 8.0, 4.0, 2.0, 1.0, 0.5, 0.25, 0.125, 2.0 / 3.0, 1.0 / 3.0, 1.0 / 6.0};

ParamInfo step(const char *name, const char *key, float def)
{
    return {name, key, 0, 100, def, U::Percent};
}

const ModulatorInfo kInfos[kModulatorKindCount] = {
    {"LFO", "lfo", 6,
     {{{"Shape", "shape", 0, 6, 0, U::Choice, false, "Sine|Triangle|Saw up|Saw down|Square|Sample and hold|Smooth random"},
       {"Rate", "rate", 0.01f, 40.0f, 1.0f, U::Hertz, true},
       {"Sync", "sync", 0, 1, 1, U::Toggle},
       {"Synced rate", "syncRate", 0, 10, 4, U::Choice, false, SYNC_CHOICES},
       {"Phase", "phase", 0, 360, 0, U::None},
       {"Polarity", "polarity", 0, 1, 0, U::Choice, false, "Bipolar|Unipolar"}}}},
    {"Steps", "steps", 19,
     {{{"Steps", "count", 1, 16, 8, U::None},
       {"Rate", "rate", 0, 10, 5, U::Choice, false, SYNC_CHOICES},
       {"Glide", "glide", 0, 100, 0, U::Percent},
       step("Step 1", "s1", 100), step("Step 2", "s2", 25), step("Step 3", "s3", 60), step("Step 4", "s4", 25),
       step("Step 5", "s5", 80), step("Step 6", "s6", 25), step("Step 7", "s7", 60), step("Step 8", "s8", 40),
       step("Step 9", "s9", 50), step("Step 10", "s10", 50), step("Step 11", "s11", 50), step("Step 12", "s12", 50),
       step("Step 13", "s13", 50), step("Step 14", "s14", 50), step("Step 15", "s15", 50), step("Step 16", "s16", 50)}}},
    {"Follower", "follower", 3,
     {{{"Attack", "attack", 1, 500, 10, U::Milliseconds, true},
       {"Release", "release", 10, 2000, 150, U::Milliseconds, true},
       {"Gain", "gain", -24, 24, 0, U::Decibels}}}},
    {"Macro", "macro", 1, {{{"Value", "value", 0, 100, 0, U::Percent}}}},
};

int choice(float value, int count)
{
    return std::clamp(static_cast<int>(std::lround(value)), 0, count - 1);
}

}

const ModulatorInfo &modulatorInfo(ModulatorKind kind)
{
    return kInfos[static_cast<int>(kind)];
}

bool modulatorFromKey(const std::string &key, ModulatorKind &kind)
{
    for (int i = 0; i < kModulatorKindCount; ++i) {
        if (key == kInfos[i].key) {
            kind = static_cast<ModulatorKind>(i);
            return true;
        }
    }
    return false;
}

DeviceSetting modulatorSetting(ModulatorKind kind)
{
    const ModulatorInfo &info = modulatorInfo(kind);
    DeviceSetting s;
    s.kind = DeviceKind::Modulator;
    s.id = info.key;
    s.name = info.name;
    for (size_t i = 0; i < info.paramCount; ++i)
        s.setValue(static_cast<uint32_t>(i), info.params[i].def);
    return s;
}

std::vector<DeviceParam> modulatorParams(ModulatorKind kind)
{
    const ModulatorInfo &info = modulatorInfo(kind);
    std::vector<DeviceParam> out;
    for (size_t i = 0; i < info.paramCount; ++i) {
        DeviceParam p = effectParam(info.params[i], static_cast<uint32_t>(i));
        p.stepped = p.stepped || (kind == ModulatorKind::Steps && i == 0);
        out.push_back(p);
    }
    return out;
}

bool modulatorBipolar(const DeviceSetting &setting)
{
    return setting.id == "lfo" && setting.value(5, 0.0) < 0.5;
}

double syncedBeats(int index)
{
    return kSynced[std::clamp(index, 0, 10)];
}

ModulatorState::ModulatorState(ModulatorKind kind) : kind_(kind)
{
    const ModulatorInfo &info = modulatorInfo(kind);
    for (size_t i = 0; i < info.paramCount; ++i)
        values_[i].store(info.params[i].def, std::memory_order_relaxed);
}

void ModulatorState::apply(const DeviceSetting &setting)
{
    const ModulatorInfo &info = modulatorInfo(kind_);
    for (size_t i = 0; i < info.paramCount; ++i) {
        const double v = setting.value(static_cast<uint32_t>(i), info.params[i].def);
        values_[i].store(static_cast<float>(std::clamp(v, static_cast<double>(info.params[i].min), static_cast<double>(info.params[i].max))),
                         std::memory_order_relaxed);
    }
    bypass_.store(setting.bypass, std::memory_order_relaxed);
}

float ModulatorState::value(const ModClock &clock) const
{
    if (bypass_.load(std::memory_order_relaxed))
        return 0.0f;
    switch (kind_) {
    case ModulatorKind::Lfo: {
        // Cycles since the song's start (its beat when synced, its seconds else), plus the phase
        const double cycles = param(2) >= 0.5f ? clock.beat / syncedBeats(choice(param(3), 11)) : clock.seconds * param(1);
        const double at = cycles + param(4) / 360.0;
        const double f = at - std::floor(at);
        const auto cycle = static_cast<int64_t>(std::floor(at));
        double v = 0.0;
        switch (choice(param(0), 7)) {
        case 0: v = std::sin(kTwoPi * f); break;
        case 1: v = f < 0.25 ? 4.0 * f : f < 0.75 ? 2.0 - 4.0 * f : 4.0 * f - 4.0; break;
        case 2: v = 2.0 * f - 1.0; break;
        case 3: v = 1.0 - 2.0 * f; break;
        case 4: v = f < 0.5 ? 1.0 : -1.0; break;
        case 5: v = 2.0 * midiHash(static_cast<uint64_t>(cycle), 11) - 1.0; break;
        default: {
            // Smooth random: a cosine between one cycle's draw and the next
            const double a = 2.0 * midiHash(static_cast<uint64_t>(cycle), 11) - 1.0;
            const double b = 2.0 * midiHash(static_cast<uint64_t>(cycle + 1), 11) - 1.0;
            const double t = 0.5 - 0.5 * std::cos(f * kTwoPi * 0.5);
            v = a + (b - a) * t;
            break;
        }
        }
        return static_cast<float>(param(5) >= 0.5f ? 0.5 * (v + 1.0) : v);
    }
    case ModulatorKind::Steps: {
        const int count = std::clamp(static_cast<int>(std::lround(param(0))), 1, 16);
        const double steps = clock.beat / syncedBeats(choice(param(1), 11));
        const auto index = static_cast<int64_t>(std::floor(steps));
        const double f = steps - std::floor(steps);
        auto valueOf = [&](int64_t i) {
            const auto s = static_cast<size_t>(((i % count) + count) % count);
            return param(3 + s) / 100.0f;
        };
        const float now = valueOf(index);
        const double glide = param(2) / 100.0;
        // Glide: the last part of each step moves to the next step's value
        if (glide > 0.0 && f > 1.0 - glide) {
            const double t = (f - (1.0 - glide)) / glide;
            return static_cast<float>(now + (valueOf(index + 1) - now) * t);
        }
        return now;
    }
    case ModulatorKind::Follower:
        return std::clamp(envelope_ * std::pow(10.0f, param(2) / 20.0f), 0.0f, 1.0f);
    case ModulatorKind::Macro:
        return param(0) / 100.0f;
    }
    return 0.0f;
}

void ModulatorState::follow(float level, double seconds)
{
    if (kind_ != ModulatorKind::Follower)
        return;
    const double time = (level > envelope_ ? param(0) : param(1)) / 1000.0;
    const auto k = static_cast<float>(1.0 - std::exp(-seconds / std::max(1e-4, time)));
    envelope_ += (level - envelope_) * k;
}

double modulatedValue(double value, const ParamCurve &curve, const ResolvedRoute *routes, size_t count, const float *values)
{
    double position = curve.position(value);
    for (size_t i = 0; i < count; ++i)
        position += static_cast<double>(routes[i].depth) * values[routes[i].slot];
    return curve.value(std::clamp(position, 0.0, 1.0));
}

}
