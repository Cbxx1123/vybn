#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "daw/model/Project.h"
#include "daw/model/ProjectStore.h"
#include "audio/Automation.h"
#include "audio/Device.h"

namespace daw {

// What the app knows about automation lanes beyond their points (audio/Automation.h): which
// parameter a lane is for, its name and its values as text, and how its 0-1 positions map to the
// parameter's values. A track's own lanes are its volume and pan; a device's are its parameters.

// The track's own parameters, described like a device's: "Volume" (linear gain 0-2, shown in dB, on
// a fader's taper), "Pan" (-1 to 1) and a send's level ("Send", linear 0-1 on a fader's taper; see Routing.h)
audio::DeviceParam trackParam(uint32_t param);
// How a lane's positions map to values: a track parameter's (volume on the fader's taper), or a
// device parameter's (its range, log sweep and steps)
audio::ParamCurve automationCurve(uint32_t trackParamId);
audio::ParamCurve automationCurve(const audio::DeviceParam &param);
// The track's own volume (linear gain) or pan as it plays at `beat`: its lane's value when the lane
// has points, else the track's gain or pan
double trackValueAt(const Track &track, uint32_t param, double beat);
// The master's volume (linear gain) as it plays at `beat`: its lane's value when the lane has points,
// else the project's masterVolume
double masterVolumeAt(const Project &project, double beat);
// A value as text: the volume in dB ("-6.0 dB", "-inf dB"), the pan as "C", "L 40" or "R 40", a
// device parameter as its device words it (formatParam)
std::string automationValueText(const audio::DeviceParam &param, double value);

// Where a lane lives: the track's own lanes, or one of its devices' (the instrument, an effect), or
// the master's: its volume (`own` and `master`; Project::masterAutomation) or a master effect's
// (a device place with `master`, which DevicePlace::effect(true, 0, i) says too)
struct AutomationPlace {
    bool own = true;    // the track's own volume and pan (the master's volume, with `master`)
    DevicePlace device; // otherwise
    bool master = false;
    bool operator==(const AutomationPlace &o) const
    {
        return own == o.own && (own ? master == o.master : device == o.device);
    }
    // The master's volume
    static AutomationPlace masterVolume() { return {true, DevicePlace(), true}; }
    // A master effect's parameters
    static AutomationPlace masterEffect(int index) { return {false, DevicePlace::effect(true, 0, index), true}; }
};

// The lanes at a place (null when the place is gone), for reading and for changing through the store.
// `track` is ignored for the master's places.
const std::vector<audio::AutomationLane> *lanesAt(const Project &project, int track, const AutomationPlace &place);
std::vector<audio::AutomationLane> *editLanesAt(ProjectStore &store, int track, const AutomationPlace &place);

// One lane of a track as the arrangement lists them: the track's own first (volume, pan), then its
// instrument's, then each effect's, in their order
struct AutomationRow {
    AutomationPlace place;
    uint32_t param = 0;
};
std::vector<AutomationRow> automationRows(const Project &project, int track);
// The master's lanes as the arrangement lists them: its volume, then each master effect's, in order
std::vector<AutomationRow> masterAutomationRows(const Project &project);

}
