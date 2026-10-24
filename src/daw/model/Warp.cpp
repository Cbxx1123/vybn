#include "daw/model/Warp.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace daw {

using audio::Warp;
using audio::WarpLoop;
using audio::WarpMarker;
using audio::WarpMode;
using audio::WarpPreserve;

namespace {

// Two markers this close in beats are one
constexpr double kSameBeat = 1e-6;

int insertMarker(Warp &w, WarpMarker marker)
{
    auto &m = w.markers;
    for (size_t i = 0; i < m.size(); ++i) {
        if (std::fabs(m[i].beat - marker.beat) < kSameBeat || std::fabs(m[i].seconds - marker.seconds) < 1e-9)
            return static_cast<int>(i);
    }
    if (m.size() >= audio::kMaxWarpMarkers)
        return -1;
    const auto at = std::upper_bound(m.begin(), m.end(), marker.beat,
                                     [](double b, const WarpMarker &k) { return b < k.beat; });
    const auto index = at - m.begin();
    m.insert(at, marker);
    return static_cast<int>(index);
}

}

const char *warpModeKey(WarpMode mode)
{
    switch (mode) {
    case WarpMode::Repitch:
        return "repitch";
    case WarpMode::Beats:
        return "beats";
    case WarpMode::Tones:
        return "tones";
    case WarpMode::Complex:
        break;
    }
    return "complex";
}

const char *warpModeName(WarpMode mode)
{
    switch (mode) {
    case WarpMode::Repitch:
        return "Repitch";
    case WarpMode::Beats:
        return "Beats";
    case WarpMode::Tones:
        return "Tones";
    case WarpMode::Complex:
        break;
    }
    return "Complex";
}

bool warpModeFromKey(const std::string &key, WarpMode &mode)
{
    for (WarpMode m : {WarpMode::Repitch, WarpMode::Beats, WarpMode::Tones, WarpMode::Complex}) {
        if (key == warpModeKey(m)) {
            mode = m;
            return true;
        }
    }
    return false;
}

const char *warpPreserveKey(WarpPreserve preserve)
{
    switch (preserve) {
    case WarpPreserve::Quarter:
        return "1/4";
    case WarpPreserve::Eighth:
        return "1/8";
    case WarpPreserve::Sixteenth:
        return "1/16";
    case WarpPreserve::Transients:
        break;
    }
    return "transients";
}

const char *warpPreserveName(WarpPreserve preserve)
{
    return preserve == WarpPreserve::Transients ? "Transients" : warpPreserveKey(preserve);
}

bool warpPreserveFromKey(const std::string &key, WarpPreserve &preserve)
{
    for (WarpPreserve p : {WarpPreserve::Transients, WarpPreserve::Quarter, WarpPreserve::Eighth, WarpPreserve::Sixteenth}) {
        if (key == warpPreserveKey(p)) {
            preserve = p;
            return true;
        }
    }
    return false;
}

const char *warpLoopKey(WarpLoop loop)
{
    switch (loop) {
    case WarpLoop::Forward:
        return "forward";
    case WarpLoop::BackAndForth:
        return "back and forth";
    case WarpLoop::Off:
        break;
    }
    return "off";
}

const char *warpLoopName(WarpLoop loop)
{
    switch (loop) {
    case WarpLoop::Forward:
        return "Forward";
    case WarpLoop::BackAndForth:
        return "Back and forth";
    case WarpLoop::Off:
        break;
    }
    return "Off";
}

bool warpLoopFromKey(const std::string &key, WarpLoop &loop)
{
    for (WarpLoop l : {WarpLoop::Off, WarpLoop::Forward, WarpLoop::BackAndForth}) {
        if (key == warpLoopKey(l)) {
            loop = l;
            return true;
        }
    }
    return false;
}

double warpPreserveBeats(WarpPreserve preserve)
{
    switch (preserve) {
    case WarpPreserve::Quarter:
        return 1.0;
    case WarpPreserve::Eighth:
        return 0.5;
    case WarpPreserve::Sixteenth:
        return 0.25;
    case WarpPreserve::Transients:
        break;
    }
    return 0.0;
}

double clipContentStart(const Clip &clip)
{
    return audio::warpBeat(clip.warp, clip.offset);
}

double clipSourceSeconds(const Clip &clip, const audio::TempoMap &timing, double beat)
{
    if (clip.warp.on)
        return audio::warpSeconds(clip.warp, clipContentStart(clip) + beat - clip.start);
    return clip.offset + timing.secondsAt(beat) - timing.secondsAt(clip.start);
}

double clipBeatAt(const Clip &clip, const audio::TempoMap &timing, double seconds)
{
    if (clip.warp.on)
        return clip.start + audio::warpBeat(clip.warp, seconds) - clipContentStart(clip);
    return timing.beatAt(timing.secondsAt(clip.start) - clip.offset + seconds);
}

double fileStartBeat(const Clip &clip, const audio::TempoMap &timing)
{
    return clipBeatAt(clip, timing, 0.0);
}

double fileEndBeat(const Clip &clip, const audio::TempoMap &timing, double fileSeconds)
{
    return clipBeatAt(clip, timing, fileSeconds);
}

double clipTempoAt(const Clip &clip, double beat)
{
    const double spb = audio::warpSecondsPerBeat(clip.warp, clipContentStart(clip) + beat - clip.start);
    return spb > 0.0 ? 60.0 / spb : 0.0;
}

Warp followTiming(const Clip &clip, const audio::TempoMap &timing, double fileSeconds)
{
    Warp w = clip.warp;
    // The timeline's seconds where the file starts, and the beats the file's start and end play at
    const double origin = timing.secondsAt(clip.start) - clip.offset;
    const double b0 = timing.beatAt(origin);
    const double b1 = fileSeconds > 0.0 ? timing.beatAt(origin + fileSeconds) : clip.start + clip.length;
    std::vector<double> beats{b0};
    const auto &points = timing.points();
    for (size_t i = 0; i < points.size(); ++i) {
        const double a = points[i].beat;
        const double e = i + 1 < points.size() ? points[i + 1].beat : 1e18;
        if (e <= b0 || a >= b1)
            continue;
        if (a > b0)
            beats.push_back(a);
        // Through a ramp, a marker every eighth of a beat (a line that short is within a fraction of a
        // millisecond of the curve)
        if (points[i].ramp && i + 1 < points.size()) {
            constexpr double kRampStep = 0.125;
            for (double x = std::ceil((std::max(a, b0) + 1e-9) / kRampStep) * kRampStep; x < std::min(e, b1); x += kRampStep)
                beats.push_back(x);
        }
    }
    if (beats.size() > 1)
        beats.push_back(b1);
    std::sort(beats.begin(), beats.end());
    beats.erase(std::unique(beats.begin(), beats.end(), [](double a, double b) { return std::fabs(a - b) < 1e-9; }),
                beats.end());
    w.bpm = std::clamp(timing.bpmAt(clip.start), audio::kMinWarpBpm, audio::kMaxWarpBpm);
    w.markers.clear();
    for (double b : beats)
        w.markers.push_back({timing.secondsAt(b) - origin, b - b0});
    audio::tidyWarp(w);
    return w;
}

void setWarp(Clip &clip, const audio::TempoMap &timing, bool on, double fileSeconds)
{
    if (on == clip.warp.on)
        return;
    if (on) {
        if (clip.warp.bpm <= 0.0)
            clip.warp = followTiming(clip, timing, fileSeconds);
        clip.warp.on = true;
    } else {
        clip.warp.on = false;
    }
    if (fileSeconds > 0.0) {
        const double end = fileEndBeat(clip, timing, fileSeconds);
        clip.length = std::max(1.0 / 64.0, std::min(clip.length, end - clip.start));
    }
}

void setClipTempo(Clip &clip, double bpm)
{
    bpm = std::clamp(bpm, audio::kMinWarpBpm, audio::kMaxWarpBpm);
    const double was = clip.warp.bpm > 0.0 ? clip.warp.bpm : 120.0;
    const double k = bpm / was;
    for (WarpMarker &m : clip.warp.markers)
        m.beat *= k;
    clip.warp.bpm = bpm;
    clip.length = std::max(1.0 / 64.0, clip.length * k);
}

void fitToLength(Clip &clip, double beats)
{
    if (!clip.warp.on || clip.length <= 0.0 || beats <= 0.0)
        return;
    const double was = clip.warp.bpm > 0.0 ? clip.warp.bpm : 120.0;
    const double bpm = std::clamp(was * beats / clip.length, audio::kMinWarpBpm, audio::kMaxWarpBpm);
    setClipTempo(clip, bpm);
}

int addWarpMarker(Clip &clip, double beat)
{
    const double content = clipContentStart(clip) + beat - clip.start;
    return insertMarker(clip.warp, {audio::warpSeconds(clip.warp, content), content});
}

int anchorClipStart(Clip &clip)
{
    auto &m = clip.warp.markers;
    for (size_t i = 0; i < m.size(); ++i) {
        if (std::fabs(m[i].seconds - clip.offset) < 1e-9)
            return static_cast<int>(i);
    }
    return insertMarker(clip.warp, {clip.offset, clipContentStart(clip)});
}

int moveWarpMarker(Clip &clip, int index, double beat)
{
    auto &m = clip.warp.markers;
    if (index < 0 || static_cast<size_t>(index) >= m.size())
        return -1;
    // The clip's start keeps its moment of the file and its beat, so only the sound between moves
    const double seconds = m[static_cast<size_t>(index)].seconds;
    const int anchor = anchorClipStart(clip);
    if (anchor < 0)
        return -1;
    for (size_t i = 0; i < m.size(); ++i) {
        if (m[i].seconds == seconds)
            index = static_cast<int>(i);
    }
    if (index == anchor)
        return index;
    const double c0 = m[static_cast<size_t>(anchor)].beat;
    double target = c0 + beat - clip.start;
    const auto i = static_cast<size_t>(index);
    const double step = 1.0 / 256.0;
    if (i > 0)
        target = std::max(target, m[i - 1].beat + step);
    if (i + 1 < m.size())
        target = std::min(target, m[i + 1].beat - step);
    if (i > 0 && i + 1 < m.size() && m[i + 1].beat - m[i - 1].beat < 2.0 * step)
        return index;
    m[i].beat = target;
    return index;
}

int slipWarpMarker(Clip &clip, int index, double seconds)
{
    auto &m = clip.warp.markers;
    if (index < 0 || static_cast<size_t>(index) >= m.size())
        return -1;
    const auto i = static_cast<size_t>(index);
    const double step = 0.001;
    if (i > 0)
        seconds = std::max(seconds, m[i - 1].seconds + step);
    if (i + 1 < m.size())
        seconds = std::min(seconds, m[i + 1].seconds - step);
    // The marker at the clip's start takes the clip's start with it
    if (std::fabs(m[i].seconds - clip.offset) < 1e-9)
        clip.offset = std::max(0.0, seconds);
    m[i].seconds = seconds;
    return index;
}

void removeWarpMarker(Clip &clip, int index)
{
    auto &m = clip.warp.markers;
    if (index >= 0 && static_cast<size_t>(index) < m.size())
        m.erase(m.begin() + index);
}

void clearWarpMarkers(Clip &clip)
{
    const WarpMarker start{clip.offset, clipContentStart(clip)};
    clip.warp.markers = {start};
}

void warpStraightFrom(Clip &clip, int index)
{
    auto &m = clip.warp.markers;
    if (index >= 0 && static_cast<size_t>(index) + 1 < m.size())
        m.erase(m.begin() + index + 1, m.end());
}

int warpMarkerNear(const Clip &clip, double contentBeat, double within)
{
    int best = -1;
    double distance = within;
    const auto &m = clip.warp.markers;
    for (size_t i = 0; i < m.size(); ++i) {
        const double d = std::fabs(m[i].beat - contentBeat);
        if (d <= distance) {
            distance = d;
            best = static_cast<int>(i);
        }
    }
    return best;
}

void quantiseWarpMarkers(Clip &clip, double grid, double strength)
{
    if (grid <= 0.0)
        return;
    const int anchor = anchorClipStart(clip);
    if (anchor < 0)
        return;
    auto &m = clip.warp.markers;
    const double c0 = m[static_cast<size_t>(anchor)].beat;
    strength = std::clamp(strength, 0.0, 1.0);
    for (size_t i = 0; i < m.size(); ++i) {
        if (static_cast<int>(i) == anchor)
            continue;
        const double at = clip.start + m[i].beat - c0;
        const double snapped = std::round(at / grid) * grid;
        m[i].beat = c0 + at + (snapped - at) * strength - clip.start;
    }
    audio::tidyWarp(clip.warp);
}

int quantiseAudio(Clip &clip, const audio::TempoMap &timing, const audio::AudioAnalysis &analysis, double grid,
                  double strength, float threshold)
{
    if (!clip.warp.on)
        setWarp(clip, timing, true, analysis.seconds());
    anchorClipStart(clip);
    const double from = clip.offset + 0.005;
    const double to = clipSourceSeconds(clip, timing, clip.end());
    int made = 0;
    for (const audio::Transient &t : analysis.transients) {
        const double s = static_cast<double>(t.frame) / analysis.sampleRate;
        if (s < from || t.strength < threshold)
            continue;
        if (s >= to)
            break;
        bool near = false;
        for (const WarpMarker &m : clip.warp.markers)
            near = near || std::fabs(m.seconds - s) < 0.01;
        if (near)
            continue;
        if (insertMarker(clip.warp, {s, audio::warpBeat(clip.warp, s)}) >= 0)
            ++made;
    }
    quantiseWarpMarkers(clip, grid, strength);
    return made;
}

WarpMode guessMode(const audio::AudioAnalysis &analysis)
{
    const double seconds = analysis.seconds();
    const double rate = seconds > 0.0 ? static_cast<double>(analysis.transients.size()) / seconds : 0.0;
    return analysis.percussive >= 0.45f && rate >= 1.0 ? WarpMode::Beats : WarpMode::Complex;
}

bool applyGuess(Clip &clip, const audio::AudioAnalysis &analysis)
{
    if (analysis.bpm <= 0.0)
        return false;
    const double at = clip.start;
    Warp w = clip.warp;
    w.on = true;
    w.bpm = std::clamp(analysis.bpm, audio::kMinWarpBpm, audio::kMaxWarpBpm);
    w.mode = guessMode(analysis);
    w.markers = {{analysis.downbeat, 0.0}};
    const double pickup = analysis.downbeat / w.secondsPerBeat(); // beats of sound before the downbeat
    clip.start = std::max(0.0, at - pickup);
    const double c0 = clip.start - at;
    clip.warp = w;
    clip.offset = std::max(0.0, audio::warpSeconds(w, c0));
    clip.length = std::max(1.0 / 64.0, audio::warpBeat(w, analysis.seconds()) - c0);
    return true;
}

bool applyGuessedTempo(Clip &clip, const audio::TempoMap &timing, const audio::AudioAnalysis &analysis)
{
    if (analysis.bpm <= 0.0)
        return false;
    const double endSeconds = std::min(clipSourceSeconds(clip, timing, clip.end()), analysis.seconds());
    const bool first = clip.warp.bpm <= 0.0;
    clip.warp.on = true;
    clip.warp.bpm = std::clamp(analysis.bpm, audio::kMinWarpBpm, audio::kMaxWarpBpm);
    if (first)
        clip.warp.mode = guessMode(analysis);
    clip.warp.markers = {{analysis.downbeat, 0.0}};
    clip.length = std::max(1.0 / 64.0, audio::warpBeat(clip.warp, endSeconds) - clipContentStart(clip));
    return true;
}

std::vector<audio::WarpSlice> warpSlices(const Clip &clip, const audio::AudioAnalysis *analysis, double fileRate,
                                         uint64_t fileFrames)
{
    std::vector<audio::WarpSlice> slices;
    const Warp &w = clip.warp;
    const double rate = fileRate > 0.0 ? fileRate : 48000.0;
    slices.push_back({0.0, audio::warpBeat(w, 0.0)});
    double grid = warpPreserveBeats(w.preserve);
    if (w.preserve == WarpPreserve::Transients && analysis) {
        for (const audio::Transient &t : analysis->transients) {
            if (t.strength < 0.1f || t.frame == 0)
                continue;
            const double frame = static_cast<double>(t.frame);
            slices.push_back({frame, audio::warpBeat(w, frame / rate)});
        }
    } else {
        if (grid <= 0.0)
            grid = 0.25; // transients not found yet
        // On the timeline's grid: content beat c plays at start + c - c0
        const double c0 = clipContentStart(clip);
        const double first = audio::warpBeat(w, 0.0);
        const double last = audio::warpBeat(w, static_cast<double>(fileFrames) / rate);
        double k = std::ceil((first - (c0 - clip.start)) / grid + 1e-9);
        for (;; k += 1.0) {
            const double c = c0 - clip.start + k * grid;
            if (c >= last)
                break;
            if (c <= first)
                continue;
            slices.push_back({audio::warpSeconds(w, c) * rate, c});
            if (slices.size() > 200000)
                break;
        }
    }
    // Rising in both, as the voice expects
    std::vector<audio::WarpSlice> kept;
    kept.reserve(slices.size());
    for (const audio::WarpSlice &s : slices) {
        if (!kept.empty() && (s.beat <= kept.back().beat || s.frame <= kept.back().frame))
            continue;
        kept.push_back(s);
    }
    return kept;
}

std::string warpBadge(const Clip &clip)
{
    std::string text = warpModeName(clip.warp.mode);
    if (clip.warp.mode != WarpMode::Repitch && clip.warp.pitch != 0.0) {
        char buffer[32];
        const double p = clip.warp.pitch;
        if (std::fabs(p - std::round(p)) < 1e-6)
            std::snprintf(buffer, sizeof buffer, " %+d st", static_cast<int>(std::lround(p)));
        else
            std::snprintf(buffer, sizeof buffer, " %+.2f st", p);
        text += buffer;
    }
    return text;
}

}
