#include "Transcribe.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <numeric>

#include "AudioAnalysis.h"
#include "Fft.h"

namespace audio {

namespace {

constexpr double kTwoPi = 6.283185307179586;

std::vector<float> monoOf(const float *interleaved, uint64_t frames, uint32_t channels)
{
    std::vector<float> mono(static_cast<size_t>(frames));
    for (uint64_t i = 0; i < frames; ++i) {
        float sum = 0.0f;
        for (uint32_t c = 0; c < channels; ++c)
            sum += interleaved[i * channels + c];
        mono[static_cast<size_t>(i)] = sum / static_cast<float>(std::max(1u, channels));
    }
    return mono;
}

std::vector<float> hann(size_t n)
{
    std::vector<float> w(n);
    for (size_t i = 0; i < n; ++i)
        w[i] = static_cast<float>(0.5 - 0.5 * std::cos(kTwoPi * static_cast<double>(i) / static_cast<double>(n)));
    return w;
}

size_t powerOfTwo(size_t n)
{
    size_t p = 1;
    while (p < n)
        p *= 2;
    return p;
}

float median(std::vector<float> values)
{
    if (values.empty())
        return 0.0f;
    const auto mid = values.begin() + static_cast<std::ptrdiff_t>(values.size() / 2);
    std::nth_element(values.begin(), mid, values.end());
    return *mid;
}

const char *kNames[12] = {"C", "C#", "D", "Eb", "E", "F", "F#", "G", "Ab", "A", "Bb", "B"};

struct Shape {
    ChordQuality quality;
    const char *suffix;
    std::vector<int> tones;
};
const std::vector<Shape> &shapes()
{
    static const std::vector<Shape> list{
        {ChordQuality::Major, "", {0, 4, 7}},           {ChordQuality::Minor, "m", {0, 3, 7}},
        {ChordQuality::Seventh, "7", {0, 4, 7, 10}},    {ChordQuality::MinorSeventh, "m7", {0, 3, 7, 10}},
        {ChordQuality::Diminished, "dim", {0, 3, 6}}, {ChordQuality::Suspended, "sus4", {0, 5, 7}},
    };
    return list;
}

}

std::vector<int> TranscribedChord::keys() const
{
    std::vector<int> out{48 + root};
    for (const Shape &s : shapes()) {
        if (s.quality != quality)
            continue;
        for (int tone : s.tones)
            out.push_back(60 + (root + tone) % 12);
    }
    std::sort(out.begin() + 1, out.end());
    return out;
}

// Drums

std::vector<TranscribedNote> transcribeDrums(const float *interleaved, uint64_t frames, uint32_t channels, double sampleRate)
{
    std::vector<TranscribedNote> notes;
    if (!interleaved || frames == 0 || sampleRate <= 0.0)
        return notes;
    const AudioAnalysis analysis = analyseSamples(interleaved, frames, channels, sampleRate);
    const std::vector<float> mono = monoOf(interleaved, frames, channels);
    const size_t n = powerOfTwo(static_cast<size_t>(0.06 * sampleRate));
    Fft fft(n);
    const std::vector<float> window = hann(n);
    std::vector<float> frame(n), re(fft.bins()), im(fft.bins());
    const double binHz = sampleRate / static_cast<double>(n);
    for (const Transient &t : analysis.transients) {
        // Hits far weaker than the loudest (a tail cut off, a bleed) aren't played
        if (t.strength < 0.1f)
            continue;
        const auto at = static_cast<size_t>(t.frame);
        for (size_t k = 0; k < n; ++k)
            frame[k] = at + k < mono.size() ? mono[at + k] * window[k] : 0.0f;
        fft.forward(frame.data(), re.data(), im.data());
        double low = 0.0, mid = 0.0, high = 0.0;
        for (size_t b = 1; b < fft.bins(); ++b) {
            const double f = static_cast<double>(b) * binHz;
            const double p = static_cast<double>(re[b]) * re[b] + static_cast<double>(im[b]) * im[b];
            if (f < 150.0)
                low += p;
            else if (f < 4000.0)
                mid += p;
            else if (f > 6000.0)
                high += p;
        }
        const double total = low + mid + high + 1e-12;
        // How long it rings: until its 5 ms level falls under a quarter of its peak (400 ms at most)
        const auto run = std::max<size_t>(1, static_cast<size_t>(0.005 * sampleRate));
        auto levelAt = [&](size_t from) {
            double sum = 0.0;
            for (size_t i = from; i < from + run && i < mono.size(); ++i)
                sum += static_cast<double>(mono[i]) * mono[i];
            return std::sqrt(sum / static_cast<double>(run));
        };
        double peak = 0.0;
        size_t peakAt = at;
        for (size_t i = at; i < at + static_cast<size_t>(0.02 * sampleRate) && i < mono.size(); i += run) {
            const double l = levelAt(i);
            if (l > peak) {
                peak = l;
                peakAt = i;
            }
        }
        double rings = 0.4;
        for (size_t i = peakAt; i < peakAt + static_cast<size_t>(0.4 * sampleRate) && i < mono.size(); i += run) {
            if (levelAt(i) < 0.25 * peak) {
                rings = static_cast<double>(i - at) / sampleRate;
                break;
            }
        }
        TranscribedNote note;
        note.start = static_cast<double>(t.frame) / sampleRate;
        note.length = 0.1;
        if (low / total > 0.5)
            note.key = kKickKey;
        else if (high / total > 0.5)
            note.key = rings > 0.15 ? kOpenHatKey : kClosedHatKey;
        else
            note.key = kSnareKey;
        note.velocity = std::clamp(0.35f + 0.65f * std::sqrt(std::clamp(t.strength, 0.0f, 1.0f)), 0.1f, 1.0f);
        notes.push_back(note);
    }
    return notes;
}

// Pitch

PitchTrack trackPitch(const float *interleaved, uint64_t frames, uint32_t channels, double sampleRate)
{
    PitchTrack track;
    if (!interleaved || frames == 0 || sampleRate <= 0.0)
        return track;
    const std::vector<float> x = monoOf(interleaved, frames, channels);
    const auto hop = std::max<size_t>(1, static_cast<size_t>(std::lround(0.005 * sampleRate)));
    track.hopSeconds = static_cast<double>(hop) / sampleRate;
    const size_t w = powerOfTwo(static_cast<size_t>(0.02 * sampleRate)); // the integration window
    const auto tauMin = std::max<size_t>(2, static_cast<size_t>(sampleRate / 1500.0));
    const auto tauMax = static_cast<size_t>(std::ceil(sampleRate / 50.0));
    const size_t n = powerOfTwo(2 * w + tauMax);
    Fft fft(n);
    std::vector<float> a(n), b(n), ar(fft.bins()), ai(fft.bins()), br(fft.bins()), bi(fft.bins()), r(n);
    std::vector<double> energy(w + tauMax + 1), cmnd(tauMax + 1);
    const size_t count = x.size() > w + tauMax ? (x.size() - w - tauMax) / hop + 1 : 0;
    track.hz.assign(count, 0.0f);
    track.level.assign(count, 0.0f);
    for (size_t f = 0; f < count; ++f) {
        const size_t s = f * hop;
        std::fill(a.begin(), a.end(), 0.0f);
        std::fill(b.begin(), b.end(), 0.0f);
        std::copy_n(x.begin() + static_cast<std::ptrdiff_t>(s), w, a.begin());
        std::copy_n(x.begin() + static_cast<std::ptrdiff_t>(s), w + tauMax, b.begin());
        // The correlation of the window with what follows it, through the FFT
        fft.forward(a.data(), ar.data(), ai.data());
        fft.forward(b.data(), br.data(), bi.data());
        for (size_t k = 0; k < fft.bins(); ++k) {
            const float re = ar[k] * br[k] + ai[k] * bi[k];
            const float im = ar[k] * bi[k] - ai[k] * br[k];
            ar[k] = re;
            ai[k] = im;
        }
        fft.inverse(ar.data(), ai.data(), r.data());
        energy[0] = 0.0;
        for (size_t j = 0; j < w + tauMax; ++j)
            energy[j + 1] = energy[j] + static_cast<double>(b[j]) * b[j];
        const double e0 = energy[w];
        track.level[f] = static_cast<float>(std::sqrt(e0 / static_cast<double>(w)));
        // The difference at each lag, normalised by its running mean; the first dip under the threshold
        double running = 0.0;
        cmnd[0] = 1.0;
        size_t found = 0;
        for (size_t tau = 1; tau <= tauMax; ++tau) {
            const double et = energy[tau + w] - energy[tau];
            const double d = std::max(0.0, e0 + et - 2.0 * r[tau]);
            running += d;
            cmnd[tau] = running > 0.0 ? d * static_cast<double>(tau) / running : 1.0;
        }
        for (size_t tau = tauMin; tau < tauMax; ++tau) {
            if (cmnd[tau] < 0.15) {
                while (tau + 1 < tauMax && cmnd[tau + 1] < cmnd[tau])
                    ++tau;
                found = tau;
                break;
            }
        }
        // None under the threshold: the deepest dip, when it is still clear (a mixture of sounds ringing)
        if (found == 0) {
            size_t deepest = tauMin;
            for (size_t tau = tauMin; tau < tauMax; ++tau)
                if (cmnd[tau] < cmnd[deepest])
                    deepest = tau;
            if (cmnd[deepest] < 0.35)
                found = deepest;
        }
        if (found == 0)
            continue;
        // Between lags: the parabola through the dip and its neighbours
        const double y0 = cmnd[found - 1], y1 = cmnd[found], y2 = cmnd[found + 1];
        const double denominator = y0 - 2.0 * y1 + y2;
        const double shift = std::fabs(denominator) > 1e-12 ? 0.5 * (y0 - y2) / denominator : 0.0;
        track.hz[f] = static_cast<float>(sampleRate / (static_cast<double>(found) + std::clamp(shift, -1.0, 1.0)));
    }
    // Quieter than 45 dB under the loudest: not voiced
    const float loudest = track.level.empty() ? 0.0f : *std::max_element(track.level.begin(), track.level.end());
    const float gate = loudest * 0.0056f;
    for (size_t f = 0; f < count; ++f)
        if (track.level[f] < gate)
            track.hz[f] = 0.0f;
    return track;
}

std::vector<TranscribedNote> transcribeMelody(const float *interleaved, uint64_t frames, uint32_t channels, double sampleRate)
{
    std::vector<TranscribedNote> notes;
    const PitchTrack track = trackPitch(interleaved, frames, channels, sampleRate);
    if (track.hz.empty())
        return notes;
    const size_t count = track.hz.size();
    // Semitones, smoothed by a median over five voiced hops
    std::vector<float> raw(count, 0.0f), midi(count, 0.0f);
    for (size_t i = 0; i < count; ++i)
        raw[i] = track.hz[i] > 0.0f ? 69.0f + 12.0f * std::log2(track.hz[i] / 440.0f) : 0.0f;
    for (size_t i = 0; i < count; ++i) {
        if (raw[i] == 0.0f)
            continue;
        std::vector<float> near;
        for (size_t j = i >= 2 ? i - 2 : 0; j <= i + 2 && j < count; ++j)
            if (raw[j] != 0.0f)
                near.push_back(raw[j]);
        midi[i] = median(near);
    }
    // New hits split notes: transients where the level really rises (sounds ringing together beat, and
    // their flux can look like a hit that isn't one)
    std::vector<bool> onset(count, false);
    const AudioAnalysis analysis = analyseSamples(interleaved, frames, channels, sampleRate);
    for (const Transient &t : analysis.transients) {
        const auto hop = static_cast<size_t>(static_cast<double>(t.frame) / sampleRate / track.hopSeconds);
        if (hop >= count)
            continue;
        float before = 1e9f, after = 0.0f;
        for (size_t i = hop >= 6 ? hop - 6 : 0; i <= hop; ++i)
            before = std::min(before, track.level[i]);
        for (size_t i = hop; i <= hop + 6 && i < count; ++i)
            after = std::max(after, track.level[i]);
        if (after > 1.5f * before)
            onset[hop] = true;
    }
    const float loudest = *std::max_element(track.level.begin(), track.level.end());

    size_t start = 0, last = 0, gap = 0, away = 0;
    std::vector<float> held; // the note's pitches so far
    bool open = false;
    auto close = [&]() {
        if (!open)
            return;
        open = false;
        const double length = static_cast<double>(last - start + 1) * track.hopSeconds;
        if (length < 0.06)
            return;
        TranscribedNote note;
        note.start = static_cast<double>(start) * track.hopSeconds;
        note.length = length;
        const float centre = median(held);
        note.key = std::clamp(static_cast<int>(std::lround(centre)), 0, 127);
        float peak = 0.0f;
        for (size_t i = start; i <= last; ++i)
            peak = std::max(peak, track.level[i]);
        const float db = 20.0f * std::log10(std::max(peak, 1e-9f) / std::max(loudest, 1e-9f));
        note.velocity = std::clamp(0.3f + 0.7f * (db + 40.0f) / 40.0f, 0.1f, 1.0f);
        // Its drift from the key, every 20 ms where it moves (its first and last 20 ms left out: a fade's
        // pitch isn't to be trusted)
        float strays = 0.0f, previous = 1e9f;
        std::vector<std::pair<double, float>> tuning;
        for (size_t i = start + 4; i + 4 <= last; i += 4) {
            if (midi[i] == 0.0f)
                continue;
            const float off = midi[i] - static_cast<float>(note.key);
            strays = std::max(strays, std::fabs(off));
            if (std::fabs(off - previous) > 0.05f) {
                tuning.push_back({static_cast<double>(i - start) * track.hopSeconds, off});
                previous = off;
            }
        }
        if (strays > 0.15f)
            note.tuning = std::move(tuning);
        notes.push_back(std::move(note));
    };
    for (size_t i = 0; i < count; ++i) {
        if (midi[i] == 0.0f) {
            if (open && ++gap > 3)
                close();
            continue;
        }
        gap = 0;
        if (!open) {
            open = true;
            start = last = i;
            held.assign(1, midi[i]);
            away = 0;
            continue;
        }
        // A jump held for three hops (not vibrato), or a new hit, starts the next note
        const float centre = median(held);
        away = std::fabs(midi[i] - centre) > 0.6f ? away + 1 : 0;
        if (away >= 3 || (onset[i] && i - start > 8)) {
            const size_t from = away >= 3 ? i - 2 : i;
            last = from > start ? from - 1 : start;
            close();
            open = true;
            start = from;
            last = i;
            held.clear();
            for (size_t j = from; j <= i; ++j)
                if (midi[j] != 0.0f)
                    held.push_back(midi[j]);
            away = 0;
            continue;
        }
        last = i;
        held.push_back(midi[i]);
    }
    close();
    // A note split where the pitch wavered or was lost for a moment (two notes of one key under 80 ms
    // apart, the second not started by a new hit) is one note again
    std::vector<TranscribedNote> joined;
    for (TranscribedNote &note : notes) {
        if (!joined.empty()) {
            TranscribedNote &before = joined.back();
            const double gap = note.start - (before.start + before.length);
            const auto hop = static_cast<size_t>(note.start / track.hopSeconds);
            bool hit = false;
            for (size_t i = hop >= 2 ? hop - 2 : 0; i <= hop + 2 && i < count; ++i)
                hit = hit || onset[i];
            if (note.key == before.key && gap < 0.08 && !hit) {
                for (const auto &[at, off] : note.tuning)
                    before.tuning.push_back({at + note.start - before.start, off});
                before.length = note.start + note.length - before.start;
                before.velocity = std::max(before.velocity, note.velocity);
                continue;
            }
        }
        joined.push_back(std::move(note));
    }
    return joined;
}

// Harmony

std::vector<TranscribedChord> transcribeHarmony(const float *interleaved, uint64_t frames, uint32_t channels, double sampleRate,
                                                const std::vector<double> &boundaries)
{
    std::vector<TranscribedChord> chords;
    if (!interleaved || frames == 0 || sampleRate <= 0.0 || boundaries.size() < 2)
        return chords;
    const std::vector<float> mono = monoOf(interleaved, frames, channels);
    const size_t n = powerOfTwo(static_cast<size_t>(0.17 * sampleRate)); // 8192 at 48 kHz
    const auto hop = std::max<size_t>(1, static_cast<size_t>(0.05 * sampleRate));
    Fft fft(n);
    const std::vector<float> window = hann(n);
    std::vector<float> frame(n), re(fft.bins()), im(fft.bins());
    const double binHz = sampleRate / static_cast<double>(n);
    // Each bin's pitch class (-1 outside 55 Hz to 2 kHz) and whether it is in the bass (under 260 Hz)
    std::vector<int> classOf(fft.bins(), -1);
    std::vector<bool> bass(fft.bins(), false);
    for (size_t b = 1; b < fft.bins(); ++b) {
        const double f = static_cast<double>(b) * binHz;
        if (f < 55.0 || f > 2000.0)
            continue;
        classOf[b] = static_cast<int>(((std::lround(69.0 + 12.0 * std::log2(f / 440.0)) % 12) + 12) % 12);
        bass[b] = f < 260.0;
    }
    struct Frame {
        double centre;
        std::array<double, 12> chroma{};
        std::array<double, 12> low{};
        double energy = 0.0;
    };
    std::vector<Frame> frames_;
    for (size_t s = 0; s < mono.size(); s += hop) {
        Frame fr;
        fr.centre = (static_cast<double>(s) + static_cast<double>(n) / 2.0) / sampleRate;
        for (size_t k = 0; k < n; ++k)
            frame[k] = s + k < mono.size() ? mono[s + k] * window[k] : 0.0f;
        fft.forward(frame.data(), re.data(), im.data());
        for (size_t b = 1; b < fft.bins(); ++b) {
            if (classOf[b] < 0)
                continue;
            const double p = std::sqrt(static_cast<double>(re[b]) * re[b] + static_cast<double>(im[b]) * im[b]);
            fr.chroma[static_cast<size_t>(classOf[b])] += p;
            if (bass[b])
                fr.low[static_cast<size_t>(classOf[b])] += p;
            fr.energy += p;
        }
        frames_.push_back(fr);
    }
    double loudest = 0.0;
    for (const Frame &fr : frames_)
        loudest = std::max(loudest, fr.energy);
    for (size_t k = 0; k + 1 < boundaries.size(); ++k) {
        std::array<double, 12> chroma{}, low{};
        double energy = 0.0;
        int used = 0;
        for (const Frame &fr : frames_) {
            if (fr.centre < boundaries[k] || fr.centre >= boundaries[k + 1])
                continue;
            for (size_t c = 0; c < 12; ++c) {
                chroma[c] += fr.chroma[c];
                low[c] += fr.low[c];
            }
            energy += fr.energy;
            ++used;
        }
        if (used == 0 || energy / used < loudest * 0.01)
            continue; // silence
        const double norm = std::sqrt(std::inner_product(chroma.begin(), chroma.end(), chroma.begin(), 0.0));
        const auto bassNote = static_cast<int>(std::max_element(low.begin(), low.end()) - low.begin());
        double best = -1.0;
        TranscribedChord chord;
        for (int root = 0; root < 12; ++root) {
            for (const Shape &shape : shapes()) {
                double dot = 0.0, weights = 0.0;
                for (int tone : shape.tones) {
                    const double w = tone == 0 ? 1.2 : 1.0;
                    dot += w * chroma[static_cast<size_t>((root + tone) % 12)];
                    weights += w * w;
                }
                double score = dot / (norm * std::sqrt(weights) + 1e-12);
                if (bassNote == root)
                    score += 0.08;
                if (shape.tones.size() == 4)
                    score -= 0.02; // a seventh only where its seventh is really there
                if (score > best) {
                    best = score;
                    chord.root = root;
                    chord.quality = shape.quality;
                    chord.name = std::string(kNames[root]) + shape.suffix;
                }
            }
        }
        chord.start = boundaries[k];
        chord.end = boundaries[k + 1];
        if (!chords.empty() && chords.back().root == chord.root && chords.back().quality == chord.quality &&
            std::fabs(chords.back().end - chord.start) < 1e-9)
            chords.back().end = chord.end;
        else
            chords.push_back(chord);
    }
    return chords;
}

}
