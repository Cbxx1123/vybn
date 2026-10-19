#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "AudioProcessor.h"
#include "Device.h"

namespace audio {

// Effects: stereo devices that sit in a chain on a track (after its instrument, before its gain
// and pan) or on the master bus (before the master gain). The built-in ones are described by an
// EffectInfo (their name and parameters, for editors and project files); a plugin effect is a
// PluginEffect. Both run through Effect, which fades in and out of bypass.

enum class EffectType { Gain, Equalizer, Filter, Gate, Compressor, Limiter, Saturation, Delay, Reverb };
constexpr int kEffectTypeCount = 9;
constexpr size_t kMaxEffectParams = 8;
constexpr size_t kMaxEffects = 8; // in one chain

// A built-in effect's parameter (its id is its index)
struct ParamInfo {
    const char *name;
    const char *key; // the parameter's name in code and tests
    float min;
    float max;
    float def;
    ParamUnit unit;
    bool logarithmic = false; // knobs sweep the range on a log scale (frequencies, times)
    const char *choices = ""; // "Low-pass|High-pass|Band-pass"
};

struct EffectInfo {
    const char *name;
    const char *key; // the device id in project files
    size_t paramCount;
    std::array<ParamInfo, kMaxEffectParams> params;
    bool hasMeter; // reports gain reduction (see Effect::meter)
};

const EffectInfo &effectInfo(EffectType type);
// The type with this key
bool effectFromKey(const std::string &key, EffectType &type);
// A built-in effect's setting with every parameter at its default
DeviceSetting effectSetting(EffectType type);
// A built-in effect's parameter as a device parameter
DeviceParam effectParam(const ParamInfo &param, uint32_t id);

// An effect on the audio thread: a Device that processes stereo in place. Bypass is an atomic,
// set from any thread; process runs on the audio thread and must not lock or allocate.
class Effect : public Device {
public:
    void setBypass(bool bypass) { bypass_.store(bypass, std::memory_order_relaxed); }
    bool bypassed() const { return bypass_.load(std::memory_order_relaxed); }
    float meter() const override { return meter_.load(std::memory_order_relaxed); }

    // Main thread, while the effect isn't being processed. Allocates what it needs.
    void prepare(double sampleRate, uint32_t maxFrames);
    // Audio thread: processes the left and right channels in place. Bypassing fades between the
    // processed and the dry sound, so it doesn't click.
    void run(float *left, float *right, uint32_t frames, const TransportState *transport = nullptr);

protected:
    virtual void onPrepare(double sampleRate, uint32_t maxFrames) = 0;
    virtual void reset() = 0;
    virtual void process(float *left, float *right, uint32_t frames) = 0;

    void setMeter(float decibels) { meter_.store(decibels, std::memory_order_relaxed); }
    double sampleRate_ = 48000.0;
    const TransportState *transport_ = nullptr; // the block's, while process runs (may be null)

private:
    std::atomic<bool> bypass_{false};
    std::atomic<float> meter_{0.0f};
    std::vector<float> dry_[2]; // while fading in or out of bypass
    bool fresh_ = true;         // not run since prepare: settles on its parameters first
    float wet_ = 1.0f;          // 0 bypassed, 1 processing
    float wetStep_ = 0.0f;
};

// A built-in effect: its parameters are atomics (ids 0, 1, ... as its EffectInfo lists them), set
// from any thread; changes to filter coefficients and levels take effect at the start of each block.
class BuiltinEffect : public Effect {
public:
    explicit BuiltinEffect(EffectType type) : type_(type) {}

    EffectType type() const { return type_; }
    DeviceKind kind() const override { return DeviceKind::Effect; }
    bool matches(const DeviceSetting &setting) const override;
    void applySetting(const DeviceSetting &setting) override;
    std::vector<DeviceParam> params() const override;
    bool hasMeter() const override { return effectInfo(type_).hasMeter; }

    void setParam(size_t index, float value);
    float param(size_t index) const { return values_[index].load(std::memory_order_relaxed); }

protected:
    float value(size_t index) const { return param(index); }

private:
    EffectType type_;
    std::array<std::atomic<float>, kMaxEffectParams> values_{};
};

// A creator for each built-in type
std::unique_ptr<BuiltinEffect> makeEffect(EffectType type);

}
