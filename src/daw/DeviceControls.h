#pragma once

#include <functional>
#include <string>
#include <vector>

#include "Project.h"
#include "audio/Device.h"
#include "ui/UI.h"

namespace daw {

// How a device's controls reach it: the project's setting (what is saved and undone) and the
// audio side's device (its parameters, a plugin's current values and words), both looked up each
// time since either may change or go away. Reading the setting uses `setting`; changing it goes
// through `edit`, which counts as an edit of the project.
struct DeviceAccess {
    std::function<const audio::DeviceSetting *()> setting;
    std::function<audio::DeviceSetting *()> edit;
    std::function<audio::Device *()> device;
};

// A parameter's value: the project's, else the device's own (a plugin's now), else its default
double paramValue(const DeviceAccess &access, const audio::DeviceParam &param);
void setParamValue(const DeviceAccess &access, const audio::DeviceParam &param, double value);
// The value as the device words it
std::string paramText(const DeviceAccess &access, const audio::DeviceParam &param, double value);
// The parameters worth showing: not hidden, not read-only
std::vector<audio::DeviceParam> shownParams(audio::Device *device);
// What kind of device a setting is, in a few words: "Library instrument", "Built-in effect",
// "Plugin"
std::string deviceKindText(const audio::DeviceSetting &setting, bool instrument);

// A control's cell: name, control and value packed close together, a few pixels apart (the
// shadows' spacing would leave gaps between them and the rows of controls).
class ParamCell : public ui::Container {
public:
    explicit ParamCell(ui::Direction direction = ui::Direction::Column) : Container(direction) {}
    float gap(const ui::Theme &) const override { return 2.0f; }
    float padding(const ui::Theme &) const override { return 0.0f; }
};

// One parameter as a control: a switch for an on/off one, a segmented control for a few choices
// or a dropdown for many, else a knob of `diameter` with the value below it. Its name above,
// cut to `nameChars` characters when longer. `showName` and `showValue` leave either out.
void addParamControl(ui::Context &context, ui::Container &parent, const DeviceAccess &access, const audio::DeviceParam &param,
                     float diameter, size_t nameChars = 14, bool showName = true, bool showValue = true);

// A plugin device's state as a sentence when it isn't playing ("Loading in its sandbox...", why
// it failed), or empty while it plays
std::string pluginStatus(audio::Device *device);

}
