#include "audio/Warp.h"

#include <algorithm>
#include <cmath>

namespace audio {

double warpSeconds(const Warp &warp, double beat)
{
    const std::vector<WarpMarker> &m = warp.markers;
    const double spb = warp.secondsPerBeat();
    if (m.empty())
        return beat * spb;
    if (m.size() == 1 || beat <= m.front().beat)
        return m.front().seconds + (beat - m.front().beat) * spb;
    if (beat >= m.back().beat)
        return m.back().seconds + (beat - m.back().beat) * spb;
    const auto next = std::upper_bound(m.begin(), m.end(), beat,
                                       [](double b, const WarpMarker &k) { return b < k.beat; });
    const WarpMarker &a = *(next - 1);
    const WarpMarker &b = *next;
    return a.seconds + (beat - a.beat) * (b.seconds - a.seconds) / (b.beat - a.beat);
}

double warpBeat(const Warp &warp, double seconds)
{
    const std::vector<WarpMarker> &m = warp.markers;
    const double spb = warp.secondsPerBeat();
    if (m.empty())
        return seconds / spb;
    if (m.size() == 1 || seconds <= m.front().seconds)
        return m.front().beat + (seconds - m.front().seconds) / spb;
    if (seconds >= m.back().seconds)
        return m.back().beat + (seconds - m.back().seconds) / spb;
    const auto next = std::upper_bound(m.begin(), m.end(), seconds,
                                       [](double s, const WarpMarker &k) { return s < k.seconds; });
    const WarpMarker &a = *(next - 1);
    const WarpMarker &b = *next;
    return a.beat + (seconds - a.seconds) * (b.beat - a.beat) / (b.seconds - a.seconds);
}

double warpSecondsPerBeat(const Warp &warp, double beat)
{
    const std::vector<WarpMarker> &m = warp.markers;
    if (m.size() < 2 || beat < m.front().beat || beat >= m.back().beat)
        return warp.secondsPerBeat();
    const int i = warpSegment(warp, beat);
    const WarpMarker &a = m[static_cast<size_t>(i)];
    const WarpMarker &b = m[static_cast<size_t>(i) + 1];
    return (b.seconds - a.seconds) / (b.beat - a.beat);
}

int warpSegment(const Warp &warp, double beat)
{
    const std::vector<WarpMarker> &m = warp.markers;
    const auto next = std::upper_bound(m.begin(), m.end(), beat,
                                       [](double b, const WarpMarker &k) { return b < k.beat; });
    return static_cast<int>(next - m.begin()) - 1;
}

void tidyWarp(Warp &warp)
{
    if (warp.bpm > 0.0)
        warp.bpm = std::clamp(warp.bpm, kMinWarpBpm, kMaxWarpBpm);
    else
        warp.bpm = 0.0;
    if (warp.bpm <= 0.0)
        warp.on = false;
    if (!std::isfinite(warp.pitch))
        warp.pitch = 0.0;
    warp.pitch = std::clamp(warp.pitch, -kMaxWarpPitch, kMaxWarpPitch);
    warp.envelope = std::clamp(std::isfinite(warp.envelope) ? warp.envelope : 100.0f, 0.0f, 100.0f);
    warp.grain = std::clamp(std::isfinite(warp.grain) ? warp.grain : 50.0f, 0.0f, 100.0f);
    std::vector<WarpMarker> &m = warp.markers;
    m.erase(std::remove_if(m.begin(), m.end(),
                           [](const WarpMarker &k) { return !std::isfinite(k.seconds) || !std::isfinite(k.beat); }),
            m.end());
    std::stable_sort(m.begin(), m.end(), [](const WarpMarker &a, const WarpMarker &b) { return a.beat < b.beat; });
    // Each kept marker comes later than the one before in both (an even stretch can't run backwards)
    std::vector<WarpMarker> kept;
    kept.reserve(std::min(m.size(), kMaxWarpMarkers));
    for (const WarpMarker &k : m) {
        if (kept.size() == kMaxWarpMarkers)
            break;
        if (!kept.empty() && (k.beat <= kept.back().beat + 1e-9 || k.seconds <= kept.back().seconds + 1e-9))
            continue;
        kept.push_back(k);
    }
    m = std::move(kept);
}

}
