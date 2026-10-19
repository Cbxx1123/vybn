#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "plugin/Slot.h"

namespace audio {

// Devices: everything that sits in a track's chain or the master's. A track's chain is its
// instrument (instrument tracks) followed by its effects; the master's is effects only. There are
// three kinds, all described and edited the same way:
//
//   Effect   a built-in effect (EQ, compressor, reverb...), id the effect's key ("compressor")
//   Sampler  a library instrument played by the Sampler, id the instrument's name
//   Plugin   a CLAP plugin, as an instrument or an effect, id its CLAP id
//   Synth    the built-in synthesizer (an instrument), id "vibin.synth"
//
// The project keeps a DeviceSetting for each; the audio side makes a Device from it (DeviceChain)
// and keeps it while the setting still matches, so a change of values never remakes it.
enum class DeviceKind { None, Effect, Sampler, Plugin, Synth };

struct DeviceSetting {
    DeviceKind kind = DeviceKind::None;
    std::string id;
    std::string name; // shown: the effect's, the instrument's, or the plugin's as it last named itself
    bool bypass = false;
    // Parameter values by id. A built-in effect has every one of its parameters (ids 0, 1, ...), a
    // sampler its own, a plugin those set on top of its state.
    plugin::ParamValues values;
    // A plugin's state (opaque, shared since it can be large): what its values started from,
    // refreshed when the project is saved. Not compared: a new state alone isn't a change.
    std::shared_ptr<const std::string> state;

    bool empty() const { return kind == DeviceKind::None; }
    double value(uint32_t paramId, double fallback) const { return plugin::valueOf(values, paramId, fallback); }
    void setValue(uint32_t paramId, double v) { plugin::setValue(values, paramId, v); }
    // "effect:compressor", "sampler:Spanish classical guitar", "plugin:org.surge-synth-team.surge-xt":
    // what the device catalogue knows it by
    std::string key() const;

    bool operator==(const DeviceSetting &other) const;
    bool operator!=(const DeviceSetting &other) const { return !(*this == other); }
};

const char *deviceKindKey(DeviceKind kind); // "effect", "sampler", "plugin", "synth", "" (project files)
bool deviceKindFromKey(const std::string &key, DeviceKind &kind);

// How a parameter's value is shown and edited
enum class ParamUnit {
    None,
    Decibels,
    Hertz,
    Milliseconds,
    Ratio,     // 4.0 shows as "4:1"
    Percent,   // 0-100
    Toggle,    // 0 or 1
    Choice,    // 0, 1, 2... into the parameter's `choices`
    Semitones, // +12 st
};

// One parameter of a device, whatever its kind, as editors show it
struct DeviceParam {
    uint32_t id = 0;
    std::string name;
    std::string module; // a group ("Filter 1", "LFO 2"); empty for none
    double min = 0.0;
    double max = 1.0;
    double def = 0.0;
    ParamUnit unit = ParamUnit::None;
    bool logarithmic = false; // knobs sweep the range on a log scale
    bool stepped = false;     // whole numbers only
    bool hidden = false;      // not shown
    bool readOnly = false;    // the device reports it (a meter); not set
    std::vector<std::string> choices; // ParamUnit::Choice's names
};

// A value as text for its parameter: "-12.0 dB", "1.2 kHz", "4:1", "35%", "On", a choice's name
std::string formatParam(const DeviceParam &param, double value);
// Between a value and a 0-1 knob position (the parameter's curve), and back
double paramToUnit(const DeviceParam &param, double value);
double paramFromUnit(const DeviceParam &param, double position);

// What every device implements. Main thread only, apart from what its subclasses say: the audio
// side is in Effect (effects) and InstrumentDevice (instruments).
class Device {
public:
    virtual ~Device() = default;

    virtual DeviceKind kind() const = 0;
    // Whether this device can take `setting`'s place: the same kind and id
    virtual bool matches(const DeviceSetting &setting) const = 0;
    // Takes the setting's bypass and values
    virtual void applySetting(const DeviceSetting &setting) = 0;
    // Once a frame (DeviceChain::collect)
    virtual void update() {}
    // Its parameters (a plugin's once it has loaded; empty until then)
    virtual std::vector<DeviceParam> params() const = 0;
    // A value as the device words it (a plugin's own text once it has said; formatParam until then)
    virtual std::string valueText(const DeviceParam &param, double value) const { return formatParam(param, value); }
    // A plugin device's slot (its state, editor and status), else nullptr
    virtual plugin::Slot *slot() { return nullptr; }
    // Frames it delays the sound by (a plugin's latency while it plays)
    virtual uint32_t latency() const { return 0; }
    // Gain reduction in decibels over the last block, for effects that have a meter
    virtual bool hasMeter() const { return false; }
    virtual float meter() const { return 0.0f; }
};

// A plugin's parameter as a device parameter
DeviceParam pluginParam(const plugin::ParamInfo &info);

}
