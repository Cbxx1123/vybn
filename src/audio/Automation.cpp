#include "Automation.h"

#include <algorithm>
#include <cmath>

namespace audio {

namespace {

// The position at `beat` along sorted points (at least one)
float positionOf(const std::vector<AutomationPoint> &points, double beat, bool stepped)
{
    if (beat <= points.front().beat)
        return points.front().value;
    if (beat >= points.back().beat)
        return points.back().value;
    const auto next = std::upper_bound(points.begin(), points.end(), beat,
                                       [](double b, const AutomationPoint &p) { return b < p.beat; });
    const AutomationPoint &b = *next;
    const AutomationPoint &a = *(next - 1);
    if (stepped || b.beat <= a.beat)
        return a.value;
    return a.value + (b.value - a.value) * static_cast<float>((beat - a.beat) / (b.beat - a.beat));
}

}

float AutomationLane::positionAt(double beat, bool stepped) const
{
    return points.empty() ? 0.0f : positionOf(points, beat, stepped);
}

AutomationLane *findAutomation(std::vector<AutomationLane> &lanes, uint32_t param)
{
    for (AutomationLane &lane : lanes) {
        if (lane.param == param)
            return &lane;
    }
    return nullptr;
}

const AutomationLane *findAutomation(const std::vector<AutomationLane> &lanes, uint32_t param)
{
    for (const AutomationLane &lane : lanes) {
        if (lane.param == param)
            return &lane;
    }
    return nullptr;
}

AutomationLane &automationFor(std::vector<AutomationLane> &lanes, uint32_t param)
{
    if (AutomationLane *lane = findAutomation(lanes, param))
        return *lane;
    lanes.push_back({param, {}});
    return lanes.back();
}

bool automated(const std::vector<AutomationLane> &lanes, uint32_t param)
{
    const AutomationLane *lane = findAutomation(lanes, param);
    return lane && !lane->points.empty();
}

size_t setAutomationPoint(AutomationLane &lane, double beat, float value, double merge)
{
    beat = std::max(0.0, beat);
    value = std::clamp(value, 0.0f, 1.0f);
    auto &points = lane.points;
    points.erase(std::remove_if(points.begin(), points.end(),
                                [beat, merge](const AutomationPoint &p) { return std::fabs(p.beat - beat) <= merge; }),
                 points.end());
    const auto at = std::upper_bound(points.begin(), points.end(), beat,
                                     [](double b, const AutomationPoint &p) { return b < p.beat; });
    return static_cast<size_t>(points.insert(at, {beat, value}) - points.begin());
}

int eraseAutomationPoints(AutomationLane &lane, double from, double to)
{
    const size_t before = lane.points.size();
    lane.points.erase(std::remove_if(lane.points.begin(), lane.points.end(),
                                     [from, to](const AutomationPoint &p) { return p.beat >= from && p.beat <= to; }),
                      lane.points.end());
    return static_cast<int>(before - lane.points.size());
}

int thinAutomationPoints(AutomationLane &lane, float tolerance, bool stepped)
{
    auto &points = lane.points;
    if (points.size() < 3)
        return 0;
    std::vector<AutomationPoint> kept{points.front()};
    for (size_t i = 1; i + 1 < points.size(); ++i) {
        const AutomationPoint &a = kept.back();
        const AutomationPoint &p = points[i];
        const AutomationPoint &b = points[i + 1];
        bool drop;
        if (stepped) {
            drop = p.value == a.value;
        } else {
            const double span = b.beat - a.beat;
            const double t = span > 0.0 ? (p.beat - a.beat) / span : 0.0;
            const double line = a.value + (b.value - a.value) * t;
            drop = std::fabs(line - p.value) <= tolerance;
        }
        if (!drop)
            kept.push_back(p);
    }
    kept.push_back(points.back());
    const int dropped = static_cast<int>(points.size() - kept.size());
    points = std::move(kept);
    return dropped;
}

AutomationWrite beginAutomationWrite(AutomationLane &lane, double beat, float value, bool stepped, float tolerance)
{
    AutomationWrite write;
    write.stepped = stepped;
    write.tolerance = tolerance;
    write.before = lane.points;
    write.value = std::clamp(value, 0.0f, 1.0f);
    beat = std::max(0.0, beat);
    write.from = beat;
    // The lane's own value up to the touch, then the written value from it
    if (!lane.points.empty() && beat >= AutomationWrite::kGap) {
        const double hold = beat - AutomationWrite::kGap;
        const float own = lane.positionAt(hold, stepped);
        eraseAutomationPoints(lane, hold, beat);
        setAutomationPoint(lane, hold, own, 0.0);
        write.from = hold;
    } else {
        eraseAutomationPoints(lane, beat, beat);
    }
    setAutomationPoint(lane, beat, write.value, 0.0);
    write.to = beat;
    return write;
}

void writeAutomation(AutomationLane &lane, AutomationWrite &write, double beat, float value)
{
    value = std::clamp(value, 0.0f, 1.0f);
    beat = std::max(0.0, beat);
    if (beat < write.to) {
        // The playhead went back: this stretch ends, and another begins there
        finishAutomationWrite(lane, write);
        write = beginAutomationWrite(lane, beat, value, write.stepped, write.tolerance);
        return;
    }
    auto &points = lane.points;
    // Whatever was after the last write, up to here, is replaced
    points.erase(std::remove_if(points.begin(), points.end(),
                                [&](const AutomationPoint &p) { return p.beat > write.to && p.beat <= beat; }),
                 points.end());
    // A value held still: the last point moves on rather than another being added
    const auto at = std::upper_bound(points.begin(), points.end(), write.to,
                                     [](double b, const AutomationPoint &p) { return b < p.beat; });
    const size_t last = static_cast<size_t>(at - points.begin());
    if (last >= 2 && points[last - 1].beat == write.to && points[last - 2].beat >= write.from &&
        points[last - 1].value == value && points[last - 2].value == value) {
        points[last - 1].beat = beat;
    } else {
        setAutomationPoint(lane, beat, value, 0.0);
    }
    write.to = beat;
    write.value = value;
}

void finishAutomationWrite(AutomationLane &lane, AutomationWrite &write)
{
    double end = write.to;
    // The lane's own value again just after the pass, where it had one (a lane that had no points
    // holds the last value written)
    if (!write.before.empty()) {
        const double back = write.to + std::max(AutomationWrite::kGap, write.returnGap);
        const bool pointThere = std::any_of(write.before.begin(), write.before.end(), [&](const AutomationPoint &p) {
            return p.beat > write.to && p.beat <= back;
        });
        if (!pointThere) {
            setAutomationPoint(lane, back, positionOf(write.before, back, write.stepped), 0.0);
            end = back;
        }
    }
    // Thinned over the pass only, so the points around it stay as they were
    auto &points = lane.points;
    const auto first = std::lower_bound(points.begin(), points.end(), write.from,
                                        [](const AutomationPoint &p, double b) { return p.beat < b; });
    const auto last = std::upper_bound(points.begin(), points.end(), end,
                                       [](double b, const AutomationPoint &p) { return b < p.beat; });
    AutomationLane pass{lane.param, std::vector<AutomationPoint>(first, last)};
    thinAutomationPoints(pass, write.tolerance, write.stepped);
    const auto at = points.erase(first, last);
    points.insert(at, pass.points.begin(), pass.points.end());
}

double ParamCurve::value(double position) const
{
    position = std::clamp(position, 0.0, 1.0);
    double v;
    if (fader)
        v = max * position * position * position;
    else if (logarithmic && min > 0.0)
        v = min * std::pow(max / min, position);
    else
        v = min + position * (max - min);
    if (stepped)
        v = std::round(v);
    return std::clamp(v, std::min(min, max), std::max(min, max));
}

double ParamCurve::position(double v) const
{
    v = std::clamp(v, std::min(min, max), std::max(min, max));
    if (fader)
        return max > 0.0 ? std::cbrt(v / max) : 0.0;
    if (logarithmic && min > 0.0)
        return std::log(v / min) / std::log(max / min);
    return max > min ? (v - min) / (max - min) : 0.0;
}

double AutomationCurve::valueAt(double beat) const
{
    return curve.value(positionOf(points, beat, curve.stepped));
}

}
