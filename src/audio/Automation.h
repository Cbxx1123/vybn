#pragma once

#include <cstdint>
#include <vector>

// Automation: a parameter's value over the song. The project keeps lanes (a track's own volume and
// pan in its Track, a device's parameters in its DeviceSetting); the audio side plays them as
// curves, which carry the parameter's range so the audio thread can turn a lane's positions into
// the values the device takes without asking it.

namespace audio {

// The ids of a track's own lanes, beside the devices' parameter ids
constexpr uint32_t kVolumeParam = 0xFFFF0001u; // the track's gain, as its fader's position
constexpr uint32_t kPanParam = 0xFFFF0002u;    // the track's pan, -1 to 1 as 0 to 1

struct AutomationPoint {
    double beat = 0.0;  // from the start of the timeline
    float value = 0.0f; // a knob's position, 0-1 along the parameter's curve

    bool operator==(const AutomationPoint &o) const { return beat == o.beat && value == o.value; }
    bool operator!=(const AutomationPoint &o) const { return !(*this == o); }
};

// One parameter's points, sorted by beat, joined by straight lines (held, for stepped parameters).
// Before the first point the lane holds its value, and after the last. A lane with no points plays
// nothing (the parameter keeps its own value) but is still kept and shown.
struct AutomationLane {
    uint32_t param = 0;
    std::vector<AutomationPoint> points;

    // The position at `beat` (needs points)
    float positionAt(double beat, bool stepped) const;

    bool operator==(const AutomationLane &o) const { return param == o.param && points == o.points; }
    bool operator!=(const AutomationLane &o) const { return !(*this == o); }
};

// The lane for a parameter among `lanes`, or null; and one made for it (appended) when there is none
AutomationLane *findAutomation(std::vector<AutomationLane> &lanes, uint32_t param);
const AutomationLane *findAutomation(const std::vector<AutomationLane> &lanes, uint32_t param);
AutomationLane &automationFor(std::vector<AutomationLane> &lanes, uint32_t param);
// Whether `param` has a lane with points (one that plays)
bool automated(const std::vector<AutomationLane> &lanes, uint32_t param);
// Editing a lane's points: a point at `beat` (replacing any within `merge` beats; points stay
// sorted), returning its index; the points from `from` to `to` removed (returning how many); and
// points a straight line through their neighbours passes within `tolerance` of dropped (after
// drawing; stepped lanes drop only repeats), returning how many went
size_t setAutomationPoint(AutomationLane &lane, double beat, float value, double merge);
int eraseAutomationPoints(AutomationLane &lane, double from, double to);
int thinAutomationPoints(AutomationLane &lane, float tolerance, bool stepped);

// Recording a control into a lane while the song plays (a "latch" pass): begun at the beat the
// control was first moved, then written each frame at the playhead until playback stops. Each write
// replaces the points since the last one with one at the playhead (a value held still moves the last
// point on rather than adding more); finishing puts the lane's own value back just after the pass
// (when it had points) and thins what was written. The playhead going back (a loop, a seek) finishes
// the stretch so far and begins another where it went.
struct AutomationWrite {
    static constexpr double kGap = 1.0 / 128.0; // beats between the lane's own value and a pass's ends

    // Beats after the pass's last point where the lane's own value is back (set before finishing): a
    // latch pass jumps back at kGap, a touch pass let go of glides back over longer
    double returnGap = kGap;
    bool stepped = false;
    float tolerance = 0.0f; // the thinning's, as thinAutomationPoints
    double from = 0.0;      // the stretch being written: its first point (the lane's own value, when it had points)
    double to = 0.0;        // and its last
    float value = 0.0f;     // the value held: written at each frame's playhead
    std::vector<AutomationPoint> before; // the lane as the stretch began, for its value after the pass
};
AutomationWrite beginAutomationWrite(AutomationLane &lane, double beat, float value, bool stepped, float tolerance);
void writeAutomation(AutomationLane &lane, AutomationWrite &write, double beat, float value);
void finishAutomationWrite(AutomationLane &lane, AutomationWrite &write);

// How a parameter's knob position maps to its value: linear or logarithmic between min and max,
// rounded for stepped parameters (choices, switches). The track's volume is a fader's cubic taper.
struct ParamCurve {
    double min = 0.0;
    double max = 1.0;
    bool logarithmic = false;
    bool stepped = false;
    bool fader = false; // volume: max * position^3

    double value(double position) const;
    double position(double value) const;
};

// A lane ready for the audio thread: its points and its parameter's curve. Made on the main thread,
// read-only after.
struct AutomationCurve {
    uint32_t param = 0;
    ParamCurve curve;
    std::vector<AutomationPoint> points; // at least one

    // The parameter's value at `beat`
    double valueAt(double beat) const;
};

// The curves of one device (or of a track's mix: volume and pan)
using AutomationCurves = std::vector<AutomationCurve>;

}
