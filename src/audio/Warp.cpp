#include "Warp.h"

#include <algorithm>
#include <cmath>

namespace audio {

namespace {

constexpr double kTiny = 1e-9;

}

const char *warpModeName(WarpMode mode)
{
    switch (mode) {
    case WarpMode::Off: return "off";
    case WarpMode::Repitch: return "repitch";
    case WarpMode::Beats: return "beats";
    case WarpMode::Tonal: return "tonal";
    }
    return "off";
}

const char *warpModeLabel(WarpMode mode)
{
    switch (mode) {
    case WarpMode::Off: return "Off";
    case WarpMode::Repitch: return "Re-pitch";
    case WarpMode::Beats: return "Beats";
    case WarpMode::Tonal: return "Tonal";
    }
    return "Off";
}

bool warpModeFromName(const std::string &name, WarpMode &mode)
{
    for (WarpMode m : {WarpMode::Off, WarpMode::Repitch, WarpMode::Beats, WarpMode::Tonal}) {
        if (name == warpModeName(m)) {
            mode = m;
            return true;
        }
    }
    return false;
}

double ClipWarp::pitch() const
{
    if (mode != WarpMode::Beats && mode != WarpMode::Tonal)
        return 1.0;
    return std::pow(2.0, static_cast<double>(transpose) / 12.0);
}

double warpBeatAt(const ClipWarp &warp, double seconds)
{
    const double perSecond = warp.bpm / 60.0;
    const std::vector<WarpMarker> &m = warp.markers;
    if (m.empty())
        return seconds * perSecond;
    if (seconds <= m.front().seconds)
        return m.front().beat + (seconds - m.front().seconds) * perSecond;
    if (seconds >= m.back().seconds)
        return m.back().beat + (seconds - m.back().seconds) * perSecond;
    const auto it = std::upper_bound(m.begin(), m.end(), seconds, [](double s, const WarpMarker &k) { return s < k.seconds; });
    const WarpMarker &b = *it;
    const WarpMarker &a = *(it - 1);
    const double t = (seconds - a.seconds) / std::max(kTiny, b.seconds - a.seconds);
    return a.beat + t * (b.beat - a.beat);
}

double warpSecondsAt(const ClipWarp &warp, double beat)
{
    const double perBeat = 60.0 / warp.bpm;
    const std::vector<WarpMarker> &m = warp.markers;
    if (m.empty())
        return beat * perBeat;
    if (beat <= m.front().beat)
        return m.front().seconds + (beat - m.front().beat) * perBeat;
    if (beat >= m.back().beat)
        return m.back().seconds + (beat - m.back().beat) * perBeat;
    const auto it = std::upper_bound(m.begin(), m.end(), beat, [](double b, const WarpMarker &k) { return b < k.beat; });
    const WarpMarker &b = *it;
    const WarpMarker &a = *(it - 1);
    const double t = (beat - a.beat) / std::max(kTiny, b.beat - a.beat);
    return a.seconds + t * (b.seconds - a.seconds);
}

double warpBpmAt(const ClipWarp &warp, double seconds)
{
    const std::vector<WarpMarker> &m = warp.markers;
    if (m.size() < 2 || seconds < m.front().seconds || seconds >= m.back().seconds)
        return warp.bpm;
    const auto it = std::upper_bound(m.begin(), m.end(), seconds, [](double s, const WarpMarker &k) { return s < k.seconds; });
    const WarpMarker &b = *it;
    const WarpMarker &a = *(it - 1);
    return (b.beat - a.beat) / std::max(kTiny, b.seconds - a.seconds) * 60.0;
}

void tidyWarp(ClipWarp &warp)
{
    if (!std::isfinite(warp.bpm))
        warp.bpm = 120.0;
    warp.bpm = std::clamp(warp.bpm, ClipWarp::kMinBpm, ClipWarp::kMaxBpm);
    if (!std::isfinite(warp.transpose))
        warp.transpose = 0.0f;
    warp.transpose = std::clamp(warp.transpose, -ClipWarp::kMaxTranspose, ClipWarp::kMaxTranspose);
    std::vector<WarpMarker> &m = warp.markers;
    m.erase(std::remove_if(m.begin(), m.end(), [](const WarpMarker &k) { return !std::isfinite(k.seconds) || !std::isfinite(k.beat); }),
            m.end());
    std::stable_sort(m.begin(), m.end(), [](const WarpMarker &a, const WarpMarker &b) { return a.seconds < b.seconds; });
    std::vector<WarpMarker> kept;
    kept.reserve(m.size());
    for (const WarpMarker &k : m) {
        if (!kept.empty() && (k.seconds <= kept.back().seconds + 1e-6 || k.beat <= kept.back().beat + 1e-6))
            continue;
        kept.push_back(k);
    }
    m = std::move(kept);
}

}
