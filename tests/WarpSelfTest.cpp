#include "WarpSelfTest.h"
#include "Harness.h"

#include <algorithm>
#include <cmath>
#include <random>
#include <string>
#include <vector>

#include "audio/Fft.h"
#include "audio/Onsets.h"
#include "audio/Stretch.h"
#include "audio/Warp.h"
#include "daw/io/ProjectFile.h"
#include "daw/model/History.h"
#include "daw/model/Project.h"
#include "daw/model/Warping.h"

namespace tests {

namespace {

constexpr double kRate = 48000.0;
constexpr double kTwoPi = 6.283185307179586;

bool near(double a, double b, double tolerance)
{
    return std::fabs(a - b) <= tolerance;
}

std::string text(double v)
{
    char buffer[48];
    std::snprintf(buffer, sizeof(buffer), "%.4g", v);
    return buffer;
}

std::vector<float> sine(double frequency, double seconds, double level = 0.5)
{
    std::vector<float> x(static_cast<size_t>(seconds * kRate));
    for (size_t i = 0; i < x.size(); ++i)
        x[i] = static_cast<float>(level * std::sin(kTwoPi * frequency * static_cast<double>(i) / kRate));
    return x;
}

// A click track: a short decaying 2 kHz burst at each time
std::vector<float> clicks(const std::vector<double> &times, double seconds)
{
    std::vector<float> x(static_cast<size_t>(seconds * kRate), 0.0f);
    for (double t : times) {
        const auto start = static_cast<size_t>(t * kRate);
        for (size_t n = 0; n < static_cast<size_t>(0.02 * kRate) && start + n < x.size(); ++n)
            x[start + n] += static_cast<float>(0.8 * std::exp(-static_cast<double>(n) / (0.003 * kRate)) *
                                               std::sin(kTwoPi * 2000.0 * static_cast<double>(n) / kRate));
    }
    return x;
}

// The frequency of a mono signal over [from, to), from its rising zero crossings
double frequencyOf(const std::vector<float> &x, size_t from, size_t to)
{
    double first = -1.0, last = -1.0;
    int crossings = 0;
    for (size_t i = std::max<size_t>(from, 1); i < std::min(to, x.size()); ++i) {
        const double a = x[i - 1], b = x[i];
        if (a <= 0.0 && b > 0.0) {
            const double t = static_cast<double>(i - 1) + a / (a - b);
            if (first < 0.0)
                first = t;
            last = t;
            ++crossings;
        }
    }
    return crossings > 1 ? (crossings - 1) * kRate / (last - first) : 0.0;
}

double rms(const std::vector<float> &x, size_t from, size_t to)
{
    double sum = 0.0;
    size_t n = 0;
    for (size_t i = from; i < std::min(to, x.size()); ++i, ++n)
        sum += static_cast<double>(x[i]) * x[i];
    return n ? std::sqrt(sum / static_cast<double>(n)) : 0.0;
}

// A straight path: the source at `speed` times the output's time, `seconds` of output
audio::TimePath straightPath(double speed, double seconds)
{
    audio::TimePath path;
    const auto steps = static_cast<size_t>(std::ceil(seconds / path.step));
    for (size_t k = 0; k <= steps; ++k)
        path.source.push_back(speed * static_cast<double>(k) * path.step);
    return path;
}

audio::StretchSource sourceOf(const std::vector<float> &x)
{
    return {x.data(), x.size(), 1, kRate};
}

}

int runWarpAudioChecks()
{
    Checker c;

    // The FFT against a direct DFT, and back
    {
        std::mt19937 random(7);
        std::uniform_real_distribution<float> noise(-1.0f, 1.0f);
        bool same = true;
        double worst = 0.0;
        for (size_t n : {size_t{16}, size_t{256}}) {
            std::vector<float> x(n), re(n / 2 + 1), im(n / 2 + 1), back(n);
            for (float &v : x)
                v = noise(random);
            audio::RealFft fft(n);
            fft.forward(x.data(), re.data(), im.data());
            for (size_t k = 0; k <= n / 2; ++k) {
                double r = 0.0, i = 0.0;
                for (size_t t = 0; t < n; ++t) {
                    r += x[t] * std::cos(kTwoPi * static_cast<double>(k * t) / static_cast<double>(n));
                    i -= x[t] * std::sin(kTwoPi * static_cast<double>(k * t) / static_cast<double>(n));
                }
                worst = std::max({worst, std::fabs(r - re[k]), std::fabs(i - im[k])});
            }
            fft.inverse(re.data(), im.data(), back.data());
            for (size_t t = 0; t < n; ++t)
                same = same && near(back[t], x[t], 1e-4);
        }
        c.check(worst < 1e-3 && same, "FFT: matches a direct DFT and comes back", "worst " + text(worst));
    }

    // Repitch: twice as fast is an octave up and half as long
    {
        const std::vector<float> x = sine(440.0, 1.0);
        const std::vector<float> y = audio::stretchAudio(sourceOf(x), straightPath(2.0, 0.5), audio::WarpMode::Repitch, 1.0, {});
        const double f = frequencyOf(y, 1000, y.size() - 1000);
        c.check(y.size() == 24000 && near(f, 880.0, 880.0 * 0.01), "Stretch: Repitch at 2x is an octave up, half as long",
                text(f) + " Hz, " + std::to_string(y.size()) + " frames");
    }

    // Tonal: half and twice the speed keep the pitch and the level
    {
        const std::vector<float> x = sine(440.0, 1.0);
        bool ok = true;
        std::string detail;
        for (double speed : {0.5, 2.0}) {
            const double seconds = 1.0 / speed;
            const std::vector<float> y = audio::stretchAudio(sourceOf(x), straightPath(speed, seconds), audio::WarpMode::Tonal, 1.0, {});
            const size_t from = y.size() / 4, to = y.size() * 3 / 4;
            const double f = frequencyOf(y, from, to);
            const double level = 20.0 * std::log10(rms(y, from, to) / rms(x, 1000, x.size() - 1000));
            ok = ok && near(static_cast<double>(y.size()), seconds * kRate, 2.0) && near(f, 440.0, 4.4) && std::fabs(level) < 1.0;
            detail += text(speed) + "x: " + text(f) + " Hz " + text(level) + " dB  ";
        }
        c.check(ok, "Stretch: Tonal at 0.5x and 2x keeps a sine's pitch and level", detail);
    }

    // A transpose of an octave doubles the frequency and keeps the length (both stretching modes)
    {
        const std::vector<float> x = sine(330.0, 1.0);
        bool ok = true;
        std::string detail;
        for (audio::WarpMode mode : {audio::WarpMode::Tonal, audio::WarpMode::Beats}) {
            const std::vector<float> y = audio::stretchAudio(sourceOf(x), straightPath(1.0, 1.0), mode, 2.0, {});
            const double f = frequencyOf(y, 6000, y.size() - 6000);
            ok = ok && y.size() == 48000 && near(f, 660.0, 6.6);
            detail += std::string(audio::warpModeLabel(mode)) + " " + text(f) + " Hz  ";
        }
        c.check(ok, "Stretch: +12 semitones doubles the frequency, same length", detail);
    }

    // Transients of a click track, each within 2 ms, nothing else
    const std::vector<double> times{0.1, 0.35, 0.6, 0.85, 1.1, 1.35, 1.6, 1.85};
    const std::vector<float> track = clicks(times, 2.0);
    {
        const audio::OnsetAnalysis onsets = audio::analyseOnsets(track.data(), track.size(), 1, kRate);
        bool ok = onsets.transients.size() == times.size();
        double worst = 0.0;
        for (size_t i = 0; ok && i < times.size(); ++i)
            worst = std::max(worst, std::fabs(onsets.transients[i] - times[i]));
        c.check(ok && worst < 0.002, "Onsets: a click track's transients, within 2 ms",
                std::to_string(onsets.transients.size()) + " found, worst " + text(worst * 1000.0) + " ms");
    }

    // Beats: slowed to 0.8x, every click starts where its transient lands, within a millisecond
    {
        const audio::OnsetAnalysis onsets = audio::analyseOnsets(track.data(), track.size(), 1, kRate);
        const std::vector<float> y = audio::stretchAudio(sourceOf(track), straightPath(0.8, 2.5), audio::WarpMode::Beats, 1.0,
                                                         onsets.transients);
        double worst = 0.0;
        for (double t : times) {
            const double expected = t / 0.8;
            const auto centre = static_cast<int64_t>(expected * kRate);
            const auto from = static_cast<size_t>(std::max<int64_t>(0, centre - static_cast<int64_t>(0.01 * kRate)));
            const auto to = std::min(y.size(), static_cast<size_t>(centre + static_cast<int64_t>(0.01 * kRate)));
            float peak = 0.0f;
            for (size_t i = from; i < to; ++i)
                peak = std::max(peak, std::fabs(y[i]));
            size_t start = from;
            while (start < to && std::fabs(y[start]) < 0.3f * peak)
                ++start;
            worst = std::max(worst, std::fabs(static_cast<double>(start) / kRate - expected));
        }
        c.check(y.size() == 120000 && worst < 0.001, "Stretch: Beats at 0.8x keeps each click on its place",
                "worst " + text(worst * 1000.0) + " ms");
    }

    // The tempo of a beat, and of a loop cut to whole bars
    {
        std::vector<double> beats;
        for (int i = 0; i < 16; ++i)
            beats.push_back(i * 0.5); // 120 BPM
        const std::vector<float> loop = clicks(beats, 8.0);
        const audio::OnsetAnalysis onsets = audio::analyseOnsets(loop.data(), loop.size(), 1, kRate);
        const double estimate = audio::estimateTempo(onsets);
        const double exact = audio::tempoFromLength(8.0, estimate);
        c.check(near(estimate, 120.0, 1.5) && near(exact, 120.0, 1e-9), "Onsets: a 120 BPM beat's tempo, and its loop's",
                text(estimate) + " estimated, " + text(exact) + " from 4 bars");
        c.check(audio::tempoFromLength(8.0, 0.0) == 0.0 && audio::tempoFromLength(7.0, 120.0) == 0.0,
                "Onsets: a length that isn't whole bars near the estimate gives no tempo");
    }

    // A tempo in a file's name
    {
        bool named = false, bare = true;
        const bool ok = audio::bpmFromName("Loop 120bpm", &named) == 120.0 && named &&
                        audio::bpmFromName("Groove 87.5 BPM") == 87.5 && audio::bpmFromName("kit_96_a", &bare) == 96.0 && !bare &&
                        audio::bpmFromName("Kick 2024") == 0.0 && audio::bpmFromName("Take 3") == 0.0 &&
                        audio::bpmFromName("Pad-140-Cm") == 140.0;
        c.check(ok, "Onsets: tempos written in names");
    }

    return c.failures;
}

int runWarpProjectChecks()
{
    Checker c;

    // The warp map and its inverse, through the markers and beyond them
    {
        audio::ClipWarp w;
        w.mode = audio::WarpMode::Beats;
        w.bpm = 120.0;
        w.markers = {{1.0, 2.0}, {2.0, 6.0}}; // 4 beats in the second between them: 240 BPM there
        bool ok = near(audio::warpBeatAt(w, 1.5), 4.0, 1e-12) && near(audio::warpBeatAt(w, 3.0), 8.0, 1e-12) &&
                  near(audio::warpBeatAt(w, 0.0), 0.0, 1e-12) && near(audio::warpBpmAt(w, 1.5), 240.0, 1e-9) &&
                  near(audio::warpBpmAt(w, 2.5), 120.0, 1e-12);
        for (double s : {-1.0, 0.3, 1.0, 1.7, 2.0, 5.25})
            ok = ok && near(audio::warpSecondsAt(w, audio::warpBeatAt(w, s)), s, 1e-12);
        c.check(ok, "Warp: beats and seconds through the markers and beyond");
        audio::ClipWarp messy = w;
        messy.markers = {{2.0, 6.0}, {1.0, 2.0}, {1.5, 1.0}, {3.0, 6.0}};
        messy.bpm = 5000.0;
        audio::tidyWarp(messy);
        c.check(messy.markers.size() == 2 && messy.markers[0].seconds == 1.0 && messy.bpm == audio::ClipWarp::kMaxBpm,
                "Warp: tidying sorts, drops markers that don't rise and clamps the tempo");
    }

    const audio::TempoMap song(120.0, 4, 4);

    // Unwarped clips map as before; warped ones through their warp, both ways
    {
        daw::Clip clip("Loop", 8.0, 4.0);
        clip.audioFile = "loop.wav";
        clip.offset = 0.5;
        bool ok = near(daw::clipFileSeconds(song, clip, 10.0), 0.5 + 1.0, 1e-12) &&
                  near(daw::clipBeatOfFile(song, clip, 1.5), 10.0, 1e-12);
        clip.warp.mode = audio::WarpMode::Tonal;
        clip.warp.bpm = 60.0; // a beat a second in the file
        ok = ok && near(daw::clipFileSeconds(song, clip, 10.0), 2.5, 1e-12) && near(daw::clipBeatOfFile(song, clip, 2.5), 10.0, 1e-12) &&
             near(daw::warpAnchor(clip), 7.5, 1e-12);
        c.check(ok, "Clip time: unwarped by the tempo map, warped by the file's beats");
    }

    // Warping on and off keeps the audio the clip covers; a new tempo keeps its start
    {
        daw::Clip clip("Loop", 4.0, 4.0); // 2 s of audio at 120
        clip.audioFile = "loop.wav";
        clip.offset = 1.0;
        daw::setWarpMode(clip, song, audio::WarpMode::Beats, 120.0);
        const bool same = near(clip.length, 4.0, 1e-12) && near(daw::clipFileSeconds(song, clip, clip.end()), 3.0, 1e-12);
        daw::setWarpBpm(clip, 60.0);
        const bool slower = near(clip.length, 2.0, 1e-12) && near(clip.offset, 1.0, 1e-12) &&
                            near(daw::clipFileSeconds(song, clip, clip.end()), 3.0, 1e-12);
        daw::setWarpMode(clip, song, audio::WarpMode::Off, 0.0);
        const bool off = near(clip.length, 4.0, 1e-12) && !clip.warped();
        c.check(same && slower && off, "Warp edits: on, a new tempo and off keep the audio under the clip",
                "length " + text(clip.length));
    }

    // Moving a marker keeps the file beat at the clip's start: the anchor holds
    {
        daw::Clip clip("Loop", 10.0, 8.0);
        clip.audioFile = "loop.wav";
        clip.warp.mode = audio::WarpMode::Beats;
        clip.warp.bpm = 120.0;
        clip.warp.markers = {{1.0, 2.0}, {2.0, 4.0}};
        clip.offset = 1.5; // file beat 3, between the markers
        const double anchor = daw::warpAnchor(clip);
        std::vector<audio::WarpMarker> moved = clip.warp.markers;
        moved[1].beat = 4.5;
        daw::setWarpMarkers(clip, moved);
        c.check(near(daw::warpAnchor(clip), anchor, 1e-9) && near(audio::warpBeatAt(clip.warp, clip.offset), 3.0, 1e-9) &&
                    near(daw::clipBeatOfFile(song, clip, 2.0), anchor + 4.5, 1e-9),
                "Warp edits: a moved marker lands where it was put, the clip's start stays", "offset " + text(clip.offset));
    }

    // Splitting and trimming a warped clip keep its sound in place
    {
        daw::Project p;
        p.timing = song;
        p.tracks.push_back({});
        daw::Track &t = p.tracks.back();
        t.kind = daw::TrackKind::Audio;
        daw::Clip clip("Loop", 4.0, 8.0);
        clip.audioFile = "loop.wav";
        clip.warp.mode = audio::WarpMode::Tonal;
        clip.warp.bpm = 90.0;
        clip.warp.markers = {{0.5, 1.0}, {3.0, 5.0}};
        t.clips.push_back(clip);
        p.selectedTrack = 0;
        p.selectedClip = 0;
        const double at = daw::clipFileSeconds(song, clip, 7.0);
        const bool split = p.splitClip(7.0);
        const daw::Clip &right = p.tracks[0].clips[1];
        const bool kept = split && near(daw::clipFileSeconds(song, right, 7.0), at, 1e-9) &&
                          near(daw::clipFileSeconds(song, right, 9.0), daw::clipFileSeconds(song, clip, 9.0), 1e-9);
        daw::Clip trimmed = clip;
        p.trimStart(trimmed, 5.0);
        const bool trim = near(trimmed.start, 5.0, 1e-12) && near(daw::clipFileSeconds(song, trimmed, 6.0), daw::clipFileSeconds(song, clip, 6.0), 1e-9);
        c.check(kept && trim, "Warp: split and trim keep a warped clip's sound where it was");
    }

    // The render's path and placing: one tempo makes it independent of where the clip is
    {
        audio::ClipWarp w;
        w.mode = audio::WarpMode::Beats;
        w.bpm = 100.0; // a 6 s file is 10 beats: 5 s at 120
        const audio::TimePath path = daw::warpPath(song, w, -0.0, 6.0);
        const bool length = near(path.duration(), 5.0, 0.006) && near(path.at(0.0), 0.0, 1e-12) && near(path.at(2.5), 3.0, 1e-9) &&
                            near(path.outputAt(3.0), 2.5, 1e-9);
        daw::Clip clip("Loop", 20.0, 4.0);
        clip.audioFile = "loop.wav";
        clip.warp = w;
        clip.offset = 1.2; // file beat 2: a second into the render at 120
        daw::Clip moved = clip;
        moved.start = 33.0;
        const bool placed = near(daw::warpRenderOffset(song, clip, w), 1.0, 1e-9) &&
                            daw::warpKey(song, w, daw::renderAnchor(song, clip)) == daw::warpKey(song, w, daw::renderAnchor(song, moved));
        audio::TempoMap ramp = song;
        ramp.setPoints({{0.0, 120.0, true}, {64.0, 140.0, false}});
        const bool varies = daw::warpKey(ramp, w, daw::renderAnchor(ramp, clip)) != daw::warpKey(ramp, w, daw::renderAnchor(ramp, moved));
        c.check(length && placed && varies, "Warp render: its path, its place, and a key that moving keeps (one tempo)");
    }

    // Snapping transients onto the grid
    {
        daw::Clip clip("Drums", 0.0, 8.0);
        clip.audioFile = "drums.wav";
        clip.warp.mode = audio::WarpMode::Beats;
        clip.warp.bpm = 120.0;
        const std::vector<audio::WarpMarker> markers = daw::snapTransients(song, clip, {0.49, 1.02, 1.03, 2.26}, 0.5);
        const bool ok = markers.size() == 3 && near(markers[0].beat, 1.0, 1e-9) && near(markers[1].beat, 2.0, 1e-9) &&
                        markers[1].seconds == 1.02 && near(markers[2].beat, 4.5, 1e-9);
        c.check(ok, "Warp: snapping transients to the grid (two on one line: the first)", std::to_string(markers.size()) + " markers");
    }

    // The file keeps a warp; an unknown mode is refused
    {
        daw::Project p;
        p.timing = song;
        p.tracks.push_back({});
        p.tracks[0].kind = daw::TrackKind::Audio;
        daw::Clip clip("Loop", 4.0, 8.0);
        clip.audioFile = "loop.wav";
        clip.warp.mode = audio::WarpMode::Beats;
        clip.warp.bpm = 93.5;
        clip.warp.transpose = -3.25f;
        clip.warp.markers = {{0.25, 0.5}, {1.75, 3.0}};
        p.tracks[0].clips.push_back(clip);
        const std::string written = daw::writeProject(p);
        daw::Project back;
        std::string error;
        const bool read = daw::readProject(written, back, error);
        const bool same = read && back.tracks.size() == 1 && back.tracks[0].clips.size() == 1 && back.tracks[0].clips[0].warp == clip.warp;
        std::string bad = written;
        bad.replace(bad.find("\"beats\""), 7, "\"rubber\"");
        daw::Project refused;
        std::string why;
        // (reading gives the tracks and clips ids, so the text written again differs by those)
        c.check(same && !daw::readProject(bad, refused, why) &&
                    why.find("warp mode") != std::string::npos,
                "File: a clip's warp round trip, an unknown mode refused",
                error + why);
        daw::Project unwarped = back;
        unwarped.tracks[0].clips[0].warp.mode = audio::WarpMode::Off;
        c.check(!daw::sameSaved(back, unwarped) && daw::writeProject(unwarped).find("warp") == std::string::npos,
                "File: an unwarped clip writes no warp, and differs from a warped one");
    }

    // The history names warp edits
    {
        daw::Project a;
        a.timing = song;
        a.tracks.push_back({});
        a.tracks[0].kind = daw::TrackKind::Audio;
        daw::Clip clip("Loop", 0.0, 4.0);
        clip.audioFile = "loop.wav";
        a.tracks[0].clips.push_back(clip);
        daw::Project on = a;
        daw::setWarpMode(on.tracks[0].clips[0], song, audio::WarpMode::Beats, 120.0);
        daw::Project marker = on;
        daw::setWarpMarkers(marker.tracks[0].clips[0], {{0.5, 1.0}});
        daw::Project moved = marker;
        daw::setWarpMarkers(moved.tracks[0].clips[0], {{0.5, 1.5}});
        daw::Project faster = on;
        daw::setWarpBpm(faster.tracks[0].clips[0], 140.0);
        daw::Project up = on;
        up.tracks[0].clips[0].warp.transpose = 2.0f;
        const bool ok = daw::describeChange(a, on) == "Warp clip" && daw::describeChange(on, a) == "Unwarp clip" &&
                        daw::describeChange(on, marker) == "Add warp marker" && daw::describeChange(marker, moved) == "Move warp marker" &&
                        daw::describeChange(marker, on) == "Delete warp marker" && daw::describeChange(on, faster) == "Change clip tempo" &&
                        daw::describeChange(on, up) == "Transpose clip" && !daw::sameContent(on, up);
        c.check(ok, "History: warp edits are named", daw::describeChange(marker, moved));
    }

    return c.failures;
}

}
