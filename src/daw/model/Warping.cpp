#include "daw/model/Warping.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace daw {

namespace {

constexpr double kPathStep = 0.005; // seconds between a render path's samples
constexpr double kMinLength = 1.0 / 64.0;

}

void setWarpMode(Clip &clip, const audio::TempoMap &timing, audio::WarpMode mode, double bpm)
{
    if (mode == clip.warp.mode)
        return;
    const double fileEnd = clipFileSeconds(timing, clip, clip.end());
    if (!clip.warp.on() && mode != audio::WarpMode::Off) {
        clip.warp.bpm = bpm > 0.0 ? bpm : timing.bpmAt(clip.start);
        audio::tidyWarp(clip.warp);
    }
    clip.warp.mode = mode;
    if (clip.warped())
        clip.length = audio::warpBeatAt(clip.warp, fileEnd) - audio::warpBeatAt(clip.warp, clip.offset);
    else
        clip.length = timing.beatAt(timing.secondsAt(clip.start) + fileEnd - clip.offset) - clip.start;
    clip.length = std::max(kMinLength, clip.length);
}

void setWarpBpm(Clip &clip, double bpm)
{
    const audio::TempoMap none; // a warped clip's mapping doesn't read the song's tempo
    const double fileEnd = clipFileSeconds(none, clip, clip.end());
    clip.warp.bpm = bpm;
    audio::tidyWarp(clip.warp);
    clip.length = std::max(kMinLength, audio::warpBeatAt(clip.warp, fileEnd) - audio::warpBeatAt(clip.warp, clip.offset));
}

void setWarpMarkers(Clip &clip, std::vector<audio::WarpMarker> markers)
{
    const double startBeat = audio::warpBeatAt(clip.warp, clip.offset);
    clip.warp.markers = std::move(markers);
    audio::tidyWarp(clip.warp);
    clip.offset = std::max(0.0, audio::warpSecondsAt(clip.warp, startBeat));
}

double warpAnchor(const Clip &clip)
{
    return clip.start - audio::warpBeatAt(clip.warp, clip.offset);
}

std::vector<audio::WarpMarker> snapTransients(const audio::TempoMap &timing, const Clip &clip,
                                              const std::vector<double> &transients, double grid, double strength)
{
    std::vector<audio::WarpMarker> markers;
    if (!clip.warped() || grid <= 0.0)
        return markers;
    strength = std::clamp(strength, 0.0, 1.0);
    const double anchor = warpAnchor(clip);
    const double from = clipFileSeconds(timing, clip, clip.start) - 1e-6;
    const double to = clipFileSeconds(timing, clip, clip.end());
    for (double t : transients) {
        if (t < from || t > to)
            continue;
        const double beat = clipBeatOfFile(timing, clip, t);
        const double snapped = beat + (std::round(beat / grid) * grid - beat) * strength;
        const double fileBeat = snapped - anchor;
        if (!markers.empty() && fileBeat <= markers.back().beat + 1e-6)
            continue; // two hits on one line: the first keeps it
        markers.push_back({t, fileBeat});
    }
    return markers;
}

double tempoForBars(const audio::TempoMap &timing, const Clip &clip, int bars)
{
    if (bars <= 0)
        return 0.0;
    const double fileStart = clip.offset;
    const double fileEnd = clipFileSeconds(timing, clip, clip.end());
    const double seconds = fileEnd - fileStart;
    if (seconds <= 0.0)
        return 0.0;
    const double beats = bars * timing.signatureAt(clip.start).barLength();
    return beats * 60.0 / seconds;
}

double renderAnchor(const audio::TempoMap &timing, const Clip &clip)
{
    // One tempo: placed so the file's start is at the song's start (any place would do)
    if (timing.points().size() == 1)
        return -audio::warpBeatAt(clip.warp, 0.0);
    return warpAnchor(clip);
}

audio::TimePath warpPath(const audio::TempoMap &timing, const audio::ClipWarp &warp, double anchor, double fileSeconds)
{
    audio::TimePath path;
    path.step = kPathStep;
    const double firstBeat = anchor + audio::warpBeatAt(warp, 0.0);
    const double lastBeat = anchor + audio::warpBeatAt(warp, fileSeconds);
    const double begin = timing.secondsAt(firstBeat);
    const double duration = timing.secondsAt(lastBeat) - begin;
    if (duration <= 0.0)
        return path;
    const auto steps = static_cast<size_t>(std::ceil(duration / kPathStep));
    path.source.resize(steps + 1);
    for (size_t k = 0; k <= steps; ++k) {
        const double beat = timing.beatAt(begin + static_cast<double>(k) * kPathStep);
        path.source[k] = audio::warpSecondsAt(warp, beat - anchor);
    }
    // The last sample sits at the file's end exactly, so the render is as long as the file plays
    path.source.back() = std::max(path.source.back(), fileSeconds);
    return path;
}

std::string warpKey(const audio::TempoMap &timing, const audio::ClipWarp &warp, double anchor)
{
    std::string key = audio::warpModeName(warp.mode);
    char text[64];
    std::snprintf(text, sizeof(text), "|%.17g|%.9g", warp.bpm, static_cast<double>(warp.transpose));
    key += text;
    for (const audio::WarpMarker &m : warp.markers) {
        std::snprintf(text, sizeof(text), "|%.17g:%.17g", m.seconds, m.beat);
        key += text;
    }
    if (timing.points().size() == 1) {
        std::snprintf(text, sizeof(text), "|t%.17g", timing.points().front().bpm);
        key += text;
        return key;
    }
    for (const audio::TempoPoint &p : timing.points()) {
        std::snprintf(text, sizeof(text), "|t%.17g:%.17g:%d", p.beat, p.bpm, p.ramp ? 1 : 0);
        key += text;
    }
    std::snprintf(text, sizeof(text), "|a%.17g", anchor);
    key += text;
    return key;
}

double warpRenderOffset(const audio::TempoMap &timing, const Clip &clip, const audio::ClipWarp &renderWarp)
{
    const double startBeat = audio::warpBeatAt(renderWarp, clip.offset);
    const double anchor = clip.start - startBeat;
    return timing.secondsAt(clip.start) - timing.secondsAt(anchor + audio::warpBeatAt(renderWarp, 0.0));
}

}
