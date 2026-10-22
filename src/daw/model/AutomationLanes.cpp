#include "daw/model/AutomationLanes.h"

#include "daw/model/Routing.h"

#include <cmath>
#include <cstdio>

namespace daw {

namespace {

constexpr double kMaxTrackGain = 2.0; // +6 dB, as the track's fader

}

audio::DeviceParam trackParam(uint32_t param)
{
    audio::DeviceParam p;
    p.id = param;
    if (param == audio::kPanParam) {
        p.name = "Pan";
        p.min = -1.0;
        p.max = 1.0;
        p.def = 0.0;
    } else if (isSendParam(param)) {
        p.name = "Send"; // the arrangement names the return
        p.min = 0.0;
        p.max = 1.0;
        p.def = 0.0;
        p.unit = audio::ParamUnit::Decibels;
    } else {
        p.name = "Volume";
        p.min = 0.0;
        p.max = kMaxTrackGain;
        p.def = 1.0;
        p.unit = audio::ParamUnit::Decibels;
    }
    return p;
}

audio::ParamCurve automationCurve(uint32_t trackParamId)
{
    audio::ParamCurve curve;
    if (trackParamId == audio::kPanParam) {
        curve.min = -1.0;
        curve.max = 1.0;
    } else if (isSendParam(trackParamId)) {
        curve.min = 0.0;
        curve.max = 1.0; // 0 dB at most
        curve.fader = true;
    } else {
        curve.min = 0.0;
        curve.max = kMaxTrackGain;
        curve.fader = true;
    }
    return curve;
}

audio::ParamCurve automationCurve(const audio::DeviceParam &param)
{
    audio::ParamCurve curve;
    curve.min = param.min;
    curve.max = param.max;
    curve.logarithmic = param.logarithmic && param.min > 0.0;
    curve.stepped = param.stepped || param.unit == audio::ParamUnit::Choice || param.unit == audio::ParamUnit::Toggle;
    return curve;
}

double trackValueAt(const Track &track, uint32_t param, double beat)
{
    const audio::AutomationLane *lane = audio::findAutomation(track.automation, param);
    if (lane && !lane->points.empty()) {
        const audio::ParamCurve curve = automationCurve(param);
        return curve.value(lane->positionAt(beat, curve.stepped));
    }
    if (isSendParam(param)) {
        const Send *send = findSend(track, sendTarget(param));
        return send ? send->level : 0.0;
    }
    return param == audio::kPanParam ? track.pan : track.gain;
}

double masterVolumeAt(const Project &project, double beat)
{
    const audio::AutomationLane *lane = audio::findAutomation(project.masterAutomation, audio::kVolumeParam);
    if (lane && !lane->points.empty()) {
        const audio::ParamCurve curve = automationCurve(audio::kVolumeParam);
        return curve.value(lane->positionAt(beat, curve.stepped));
    }
    return project.masterVolume;
}

std::string automationValueText(const audio::DeviceParam &param, double value)
{
    char text[32];
    if (param.id == audio::kVolumeParam || isSendParam(param.id)) {
        if (value <= 1e-5)
            return "-inf dB";
        std::snprintf(text, sizeof text, "%.1f dB", 20.0 * std::log10(value));
        return text;
    }
    if (param.id == audio::kPanParam) {
        const int amount = static_cast<int>(std::lround(std::fabs(value) * 100.0));
        if (amount == 0)
            return "C";
        std::snprintf(text, sizeof text, "%s %d", value < 0.0 ? "L" : "R", amount);
        return text;
    }
    return audio::formatParam(param, value);
}

const std::vector<audio::AutomationLane> *lanesAt(const Project &project, int track, const AutomationPlace &place)
{
    if (place.own && place.master)
        return &project.masterAutomation;
    if (place.own) {
        const Track *t = project.track(track);
        return t ? &t->automation : nullptr;
    }
    const audio::DeviceSetting *device = project.device(place.device);
    return device && !device->empty() ? &device->automation : nullptr;
}

std::vector<audio::AutomationLane> *editLanesAt(ProjectStore &store, int track, const AutomationPlace &place)
{
    if (place.own && place.master)
        return &store.editMasterAutomation();
    if (place.own)
        return store.project().track(track) ? &store.editTrack(track).automation : nullptr;
    audio::DeviceSetting *device = store.editDevice(place.device);
    return device && !device->empty() ? &device->automation : nullptr;
}

std::vector<AutomationRow> automationRows(const Project &project, int track)
{
    std::vector<AutomationRow> rows;
    const Track *t = project.track(track);
    if (!t)
        return rows;
    for (const audio::AutomationLane &lane : t->automation)
        rows.push_back({AutomationPlace{}, lane.param});
    if (t->kind == TrackKind::Instrument && t->hasInstrument()) {
        for (const audio::AutomationLane &lane : t->instrument.automation)
            rows.push_back({AutomationPlace{false, DevicePlace::instrumentOf(track)}, lane.param});
    }
    for (size_t i = 0; i < t->effects.size(); ++i) {
        for (const audio::AutomationLane &lane : t->effects[i].automation)
            rows.push_back({AutomationPlace{false, DevicePlace::effect(false, track, static_cast<int>(i))}, lane.param});
    }
    return rows;
}

std::vector<AutomationRow> masterAutomationRows(const Project &project)
{
    std::vector<AutomationRow> rows;
    for (const audio::AutomationLane &lane : project.masterAutomation)
        rows.push_back({AutomationPlace::masterVolume(), lane.param});
    for (size_t i = 0; i < project.masterEffects.size(); ++i) {
        for (const audio::AutomationLane &lane : project.masterEffects[i].automation)
            rows.push_back({AutomationPlace::masterEffect(static_cast<int>(i)), lane.param});
    }
    return rows;
}

}
