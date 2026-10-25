#include "Device.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace audio {

std::string DeviceSetting::key() const
{
    return kind == DeviceKind::None ? std::string() : std::string(deviceKindKey(kind)) + ":" + id;
}

bool DeviceSetting::operator==(const DeviceSetting &other) const
{
    return kind == other.kind && id == other.id && bypass == other.bypass && sidechain == other.sidechain &&
           values == other.values && automation == other.automation && modulation == other.modulation;
}

const char *deviceKindKey(DeviceKind kind)
{
    switch (kind) {
    case DeviceKind::Effect: return "effect";
    case DeviceKind::Sampler: return "sampler";
    case DeviceKind::Plugin: return "plugin";
    case DeviceKind::Synth: return "synth";
    case DeviceKind::MidiEffect: return "midi";
    case DeviceKind::Modulator: return "modulator";
    case DeviceKind::None: break;
    }
    return "";
}

bool deviceKindFromKey(const std::string &key, DeviceKind &kind)
{
    for (DeviceKind k : {DeviceKind::Effect, DeviceKind::Sampler, DeviceKind::Plugin, DeviceKind::Synth, DeviceKind::MidiEffect,
                         DeviceKind::Modulator}) {
        if (key == deviceKindKey(k)) {
            kind = k;
            return true;
        }
    }
    return false;
}

std::string formatParam(const DeviceParam &param, double value)
{
    char text[64];
    switch (param.unit) {
    case ParamUnit::Decibels:
        std::snprintf(text, sizeof text, "%.1f dB", value);
        break;
    case ParamUnit::Hertz:
        if (value >= 1000.0)
            std::snprintf(text, sizeof text, "%.2f kHz", value / 1000.0);
        else if (value >= 10.0)
            std::snprintf(text, sizeof text, "%.0f Hz", value);
        else if (value >= 1.0)
            std::snprintf(text, sizeof text, "%.1f Hz", value);
        else
            std::snprintf(text, sizeof text, "%.2f Hz", value);
        break;
    case ParamUnit::Milliseconds:
        if (value >= 1000.0)
            std::snprintf(text, sizeof text, "%.2f s", value / 1000.0);
        else if (value >= 10.0)
            std::snprintf(text, sizeof text, "%.0f ms", value);
        else
            std::snprintf(text, sizeof text, "%.1f ms", value);
        break;
    case ParamUnit::Ratio:
        std::snprintf(text, sizeof text, "%.1f:1", value);
        break;
    case ParamUnit::Percent:
        std::snprintf(text, sizeof text, "%.0f%%", value);
        break;
    case ParamUnit::Semitones:
        std::snprintf(text, sizeof text, "%+.0f st", value);
        break;
    case ParamUnit::Toggle:
        return value >= 0.5 ? "On" : "Off";
    case ParamUnit::Choice: {
        const long i = std::lround(value);
        return i >= 0 && static_cast<size_t>(i) < param.choices.size() ? param.choices[static_cast<size_t>(i)] : std::string();
    }
    case ParamUnit::None:
        if (param.stepped)
            std::snprintf(text, sizeof text, "%.0f", value);
        else
            std::snprintf(text, sizeof text, "%.2f", value);
        break;
    }
    return text;
}

double paramToUnit(const DeviceParam &param, double value)
{
    value = std::clamp(value, param.min, param.max);
    if (param.logarithmic && param.min > 0.0)
        return std::log(value / param.min) / std::log(param.max / param.min);
    return param.max > param.min ? (value - param.min) / (param.max - param.min) : 0.0;
}

double paramFromUnit(const DeviceParam &param, double position)
{
    position = std::clamp(position, 0.0, 1.0);
    double value;
    if (param.logarithmic && param.min > 0.0)
        value = param.min * std::pow(param.max / param.min, position);
    else
        value = param.min + position * (param.max - param.min);
    if (param.stepped || param.unit == ParamUnit::Choice || param.unit == ParamUnit::Toggle)
        value = std::round(value);
    return std::clamp(value, param.min, param.max);
}

DeviceParam pluginParam(const plugin::ParamInfo &info)
{
    DeviceParam p;
    p.id = info.id;
    p.name = info.name;
    p.module = info.module;
    p.min = info.min;
    p.max = info.max;
    p.def = info.def;
    p.stepped = info.stepped();
    p.hidden = info.hidden();
    p.readOnly = info.readOnly();
    // A stepped 0-1 parameter is a switch
    if (p.stepped && p.min == 0.0 && p.max == 1.0)
        p.unit = ParamUnit::Toggle;
    return p;
}

}
