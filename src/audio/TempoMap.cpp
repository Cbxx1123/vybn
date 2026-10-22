#include "audio/TempoMap.h"

#include <algorithm>
#include <cmath>

namespace audio {

namespace {

// Beats computed by adding bar lengths miss a bar line by a rounding error; this much is on it
constexpr double kEpsilon = 1e-9;

int allowedDenominator(int d)
{
    if (d < 3)
        return 2;
    if (d < 6)
        return 4;
    if (d < 12)
        return 8;
    return 16;
}

}

TempoMap::TempoMap(double bpm, int numerator, int denominator)
{
    points_ = {{0.0, std::clamp(bpm, kMinBpm, kMaxBpm), false}};
    signatures_ = {{0, std::clamp(numerator, 1, 16), allowedDenominator(denominator)}};
    rebuild();
}

void TempoMap::setPoints(std::vector<TempoPoint> points)
{
    points.erase(std::remove_if(points.begin(), points.end(),
                                [](const TempoPoint &p) { return !std::isfinite(p.beat) || !std::isfinite(p.bpm); }),
                 points.end());
    for (TempoPoint &p : points) {
        p.beat = std::max(0.0, p.beat);
        p.bpm = std::clamp(p.bpm, kMinBpm, kMaxBpm);
    }
    std::stable_sort(points.begin(), points.end(), [](const TempoPoint &a, const TempoPoint &b) { return a.beat < b.beat; });
    points.erase(std::unique(points.begin(), points.end(), [](const TempoPoint &a, const TempoPoint &b) { return a.beat == b.beat; }),
                 points.end());
    if (points.empty())
        points.push_back({});
    if (points.front().beat > 0.0)
        points.insert(points.begin(), {0.0, points.front().bpm, false});
    points_ = std::move(points);
    rebuild();
}

void TempoMap::setSignatures(std::vector<Signature> signatures)
{
    for (Signature &s : signatures) {
        s.bar = std::max(0, s.bar);
        s.numerator = std::clamp(s.numerator, 1, 16);
        s.denominator = allowedDenominator(s.denominator);
    }
    std::stable_sort(signatures.begin(), signatures.end(), [](const Signature &a, const Signature &b) { return a.bar < b.bar; });
    signatures.erase(std::unique(signatures.begin(), signatures.end(), [](const Signature &a, const Signature &b) { return a.bar == b.bar; }),
                     signatures.end());
    if (signatures.empty())
        signatures.push_back({});
    if (signatures.front().bar > 0) {
        Signature first = signatures.front();
        first.bar = 0;
        signatures.insert(signatures.begin(), first);
    }
    signatures_ = std::move(signatures);
    rebuild();
}

void TempoMap::setTempo(double bpm)
{
    points_.front().bpm = std::clamp(bpm, kMinBpm, kMaxBpm);
    rebuild();
}

void TempoMap::rebuild()
{
    pointSeconds_.assign(points_.size(), 0.0);
    for (size_t i = 1; i < points_.size(); ++i)
        pointSeconds_[i] = pointSeconds_[i - 1] + segmentSeconds(i - 1, points_[i].beat - points_[i - 1].beat);
    signatureBeats_.assign(signatures_.size(), 0.0);
    for (size_t i = 1; i < signatures_.size(); ++i)
        signatureBeats_[i] = signatureBeats_[i - 1] + (signatures_[i].bar - signatures_[i - 1].bar) * signatures_[i - 1].barLength();
}

double TempoMap::rampSlope(size_t i) const
{
    if (!points_[i].ramp || i + 1 >= points_.size())
        return 0.0;
    return (points_[i + 1].bpm - points_[i].bpm) / (points_[i + 1].beat - points_[i].beat);
}

// A held tempo is 60/bpm seconds a beat. In a ramp the tempo is b0 + k*x after x beats, and the time
// is the integral of 60/(b0 + k*x): 60/k * ln(1 + k*x/b0), which turns back as b0/k * (e^(k*t/60) - 1).
double TempoMap::segmentSeconds(size_t i, double beats) const
{
    const double k = rampSlope(i), b0 = points_[i].bpm;
    if (std::fabs(k) < 1e-12)
        return beats * 60.0 / b0;
    return 60.0 / k * std::log1p(k * beats / b0);
}

double TempoMap::segmentBeats(size_t i, double seconds) const
{
    const double k = rampSlope(i), b0 = points_[i].bpm;
    if (std::fabs(k) < 1e-12)
        return seconds * b0 / 60.0;
    return b0 / k * std::expm1(k * seconds / 60.0);
}

size_t TempoMap::pointAt(double beat) const
{
    const auto it = std::upper_bound(points_.begin(), points_.end(), beat, [](double b, const TempoPoint &p) { return b < p.beat; });
    return it == points_.begin() ? 0 : static_cast<size_t>(it - points_.begin()) - 1;
}

double TempoMap::bpmAt(double beat) const
{
    if (beat <= 0.0)
        return points_.front().bpm;
    const size_t i = pointAt(beat);
    return points_[i].bpm + rampSlope(i) * (beat - points_[i].beat);
}

double TempoMap::slopeAt(double beat) const
{
    return beat < 0.0 ? 0.0 : rampSlope(pointAt(beat));
}

double TempoMap::secondsAt(double beat) const
{
    if (beat <= 0.0)
        return beat * 60.0 / points_.front().bpm;
    const size_t i = pointAt(beat);
    return pointSeconds_[i] + segmentSeconds(i, beat - points_[i].beat);
}

double TempoMap::beatAt(double seconds) const
{
    if (seconds <= 0.0)
        return seconds * points_.front().bpm / 60.0;
    const auto it = std::upper_bound(pointSeconds_.begin(), pointSeconds_.end(), seconds);
    const size_t i = static_cast<size_t>(it - pointSeconds_.begin()) - 1;
    return points_[i].beat + segmentBeats(i, seconds - pointSeconds_[i]);
}

size_t TempoMap::signatureIndexAt(int bar) const
{
    const auto it = std::upper_bound(signatures_.begin(), signatures_.end(), bar, [](int b, const Signature &s) { return b < s.bar; });
    return it == signatures_.begin() ? 0 : static_cast<size_t>(it - signatures_.begin()) - 1;
}

double TempoMap::barStart(int bar) const
{
    const size_t i = signatureIndexAt(bar);
    return signatureBeats_[i] + (bar - signatures_[i].bar) * signatures_[i].barLength();
}

int TempoMap::barAt(double beat) const
{
    const auto it = std::upper_bound(signatureBeats_.begin(), signatureBeats_.end(), beat + kEpsilon);
    const size_t i = it == signatureBeats_.begin() ? 0 : static_cast<size_t>(it - signatureBeats_.begin()) - 1;
    const double bars = (beat - signatureBeats_[i]) / signatures_[i].barLength();
    return signatures_[i].bar + static_cast<int>(std::floor(bars + kEpsilon));
}

BarPosition TempoMap::position(double beat) const
{
    BarPosition p;
    p.bar = barAt(beat);
    const Signature &s = signatureAtBar(p.bar);
    const double units = std::max(0.0, (beat - barStart(p.bar)) / s.unit());
    p.unit = std::min(static_cast<int>(std::floor(units + kEpsilon)), s.numerator - 1);
    p.fraction = std::clamp(units - p.unit, 0.0, 1.0);
    if (p.fraction < kEpsilon)
        p.fraction = 0.0;
    return p;
}

double TempoMap::nearestBar(double beat) const
{
    const int bar = barAt(beat);
    const double a = barStart(bar), b = barStart(bar + 1);
    return beat - a <= b - beat ? a : b;
}

}
