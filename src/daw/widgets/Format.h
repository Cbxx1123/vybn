#pragma once

// Text and scale conversions shared by the DAW's views

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>

#include "audio/TempoMap.h"

namespace daw {

// printf-style formatting of one number
inline std::string format(const char *pattern, double value)
{
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), pattern, value);
    return buffer;
}

inline float toDecibels(float gain)
{
    return gain > 1e-6f ? 20.0f * std::log10(gain) : -120.0f;
}

inline std::string decibels(float gain)
{
    const float db = toDecibels(gain);
    return db <= -90.0f ? std::string("-inf") : format("%+.1f", db);
}

// Bars, units of the signature (beats in 4/4, eighths in 6/8) and ticks (960 per unit), 1-based
inline std::string barsBeats(double beats, const audio::TempoMap &timing)
{
    const audio::BarPosition p = timing.position(std::max(0.0, beats));
    const int ticks = std::min(959, static_cast<int>(p.fraction * 960.0));
    char buffer[48];
    std::snprintf(buffer, sizeof(buffer), "%d.%d.%03d", p.bar + 1, p.unit + 1, ticks);
    return buffer;
}

inline std::string clockTime(double seconds)
{
    seconds = std::max(0.0, seconds);
    const int minutes = static_cast<int>(seconds / 60.0);
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%d:%06.3f", minutes, seconds - minutes * 60.0);
    return buffer;
}

// "C", "L40", "R25" for a pan position from -1 to 1
inline std::string panText(float pan)
{
    const int amount = static_cast<int>(std::lround(std::abs(pan) * 100.0f));
    if (amount == 0)
        return "C";
    return (pan < 0.0f ? "L" : "R") + std::to_string(amount);
}

// A fader's travel (0-1) against linear gain, on a cubic taper like a mixing desk's: most of the
// travel covers the useful range near unity. With maxGain 2, unity sits at about 0.79.
inline float faderGain(float position, float maxGain)
{
    position = std::clamp(position, 0.0f, 1.0f);
    return maxGain * position * position * position;
}

inline float faderPosition(float gain, float maxGain)
{
    return std::cbrt(std::clamp(gain / maxGain, 0.0f, 1.0f));
}

}
