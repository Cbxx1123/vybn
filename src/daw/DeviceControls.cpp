#include "DeviceControls.h"

#include <cmath>

#include "Widgets.h"

namespace daw {

using namespace ui;

double paramValue(const DeviceAccess &access, const audio::DeviceParam &param)
{
    double fallback = param.def;
    if (audio::Device *d = access.device ? access.device() : nullptr)
        if (plugin::Slot *slot = d->slot())
            if (const plugin::ParamInfo *info = slot->param(param.id))
                fallback = info->value;
    const audio::DeviceSetting *s = access.setting ? access.setting() : nullptr;
    return s ? s->value(param.id, fallback) : fallback;
}

void setParamValue(const DeviceAccess &access, const audio::DeviceParam &param, double value)
{
    const double clamped = std::clamp(value, param.min, param.max);
    // Compared first: a control that hasn't moved edits nothing (NaN stands for "not set")
    const audio::DeviceSetting *now = access.setting ? access.setting() : nullptr;
    if (now && now->value(param.id, std::nan("")) == clamped)
        return;
    if (audio::DeviceSetting *s = access.edit ? access.edit() : nullptr)
        s->setValue(param.id, clamped);
}

std::string paramText(const DeviceAccess &access, const audio::DeviceParam &param, double value)
{
    audio::Device *d = access.device ? access.device() : nullptr;
    return d ? d->valueText(param, value) : audio::formatParam(param, value);
}

std::vector<audio::DeviceParam> shownParams(audio::Device *device)
{
    std::vector<audio::DeviceParam> out;
    if (!device)
        return out;
    for (audio::DeviceParam &p : device->params())
        if (!p.hidden && !p.readOnly)
            out.push_back(std::move(p));
    return out;
}

std::string deviceKindText(const audio::DeviceSetting &setting, bool instrument)
{
    switch (setting.kind) {
    case audio::DeviceKind::Sampler: return "Library instrument";
    case audio::DeviceKind::Effect: return "Built-in effect";
    case audio::DeviceKind::Plugin: return instrument ? "Plugin instrument" : "Plugin effect";
    case audio::DeviceKind::Synth: return "Built-in instrument";
    case audio::DeviceKind::None: break;
    }
    return instrument ? "Instrument" : "Effect";
}

void addParamControl(Context &context, Container &parent, const DeviceAccess &access, const audio::DeviceParam &param, float diameter,
                     size_t nameChars, bool showName, bool showValue)
{
    auto &cell = parent.add<ParamCell>(Direction::Column);
    cell.setAlign(Align::Center);
    const std::string name = param.name.size() > nameChars ? param.name.substr(0, nameChars - 1) + "." : param.name;

    if (param.unit == audio::ParamUnit::Toggle) {
        cell.add<SmallToggle>(name, [access, param] { return paramValue(access, param) >= (param.min + param.max) * 0.5; },
                              [access, param](bool on) { setParamValue(access, param, on ? param.max : param.min); });
        return;
    }
    if (showName)
        cell.add<Label>(name, TextStyle::Caption, TextTone::Secondary);
    if (param.unit == audio::ParamUnit::Choice && !param.choices.empty()) {
        auto selected = [access, param] { return static_cast<int>(std::lround(paramValue(access, param) - param.min)); };
        auto choose = [access, param](int i) { setParamValue(access, param, param.min + i); };
        if (param.choices.size() <= 4) {
            auto &choice = cell.add<SegmentedControl>(param.choices, selected(), choose);
            choice.setSize(Size::Small);
            SegmentedControl *raw = &choice;
            cell.add<FrameHook>([selected, raw](const Input &) { raw->setSelected(selected()); });
        } else {
            auto &choice = cell.add<Dropdown>(context, param.choices, selected(), choose);
            choice.setSize(Size::Small);
            Dropdown *raw = &choice;
            cell.add<FrameHook>([selected, raw](const Input &) { raw->setSelected(selected()); });
        }
        return;
    }
    auto &knob = cell.add<Knob>(static_cast<float>(audio::paramToUnit(param, paramValue(access, param))),
                                [access, param](float position) { setParamValue(access, param, audio::paramFromUnit(param, position)); });
    knob.setDiameter(diameter);
    Knob *raw = &knob;
    cell.add<FrameHook>(
        [access, param, raw](const Input &) { raw->setValue(static_cast<float>(audio::paramToUnit(param, paramValue(access, param)))); });
    if (showValue)
        cell.add<LiveLabel>([access, param] { return paramText(access, param, paramValue(access, param)); }, TextStyle::Caption,
                            TextTone::Muted);
}

std::string pluginStatus(audio::Device *device)
{
    plugin::Slot *s = device ? device->slot() : nullptr;
    if (!device || (device->kind() == audio::DeviceKind::Plugin && !s))
        return device ? "Plugins aren't available here" : "Starting...";
    if (!s)
        return {};
    using State = plugin::Slot::State;
    switch (s->state()) {
    case State::Ready: return {};
    case State::Loading: return "Loading in its sandbox...";
    case State::Failed: return s->message() + ".";
    default:
        return s->message() + ". It is silent until reloaded. A plugin that keeps presets or a database in your documents may need that "
                              "folder: Settings > Plugins, Allow writing.";
    }
}

}
