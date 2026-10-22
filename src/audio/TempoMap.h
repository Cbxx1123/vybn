#pragma once

#include <cstdint>
#include <vector>

// The song's tempo and metre over the timeline. Positions everywhere are in beats (quarter notes);
// the map turns them into seconds and back, and into bars. The project owns one (Project::timing);
// the audio thread reads an immutable copy the transport publishes, so every query is a binary
// search and a closed form, without allocating.

namespace audio {

// A tempo from `beat` on: held until the next point, or with `ramp` changing in a straight line
// (BPM against beats) to the next point's tempo
struct TempoPoint {
    double beat = 0.0;
    double bpm = 120.0;
    bool ramp = false;

    bool operator==(const TempoPoint &o) const { return beat == o.beat && bpm == o.bpm && ramp == o.ramp; }
    bool operator!=(const TempoPoint &o) const { return !(*this == o); }
};

// A time signature from bar `bar` (0-based) on. A bar is numerator units of a 1/denominator note,
// so 6/8 is 3 beats long.
struct Signature {
    int bar = 0;
    int numerator = 4;
    int denominator = 4; // 2, 4, 8 or 16

    double unit() const { return 4.0 / denominator; } // beats in one of the signature's units
    double barLength() const { return numerator * unit(); }
    bool operator==(const Signature &o) const { return bar == o.bar && numerator == o.numerator && denominator == o.denominator; }
    bool operator!=(const Signature &o) const { return !(*this == o); }
};

// A beat as bars and units of the signature, 0-based; `fraction` is how far into the unit (0-1)
struct BarPosition {
    int bar = 0;
    int unit = 0;
    double fraction = 0.0;
};

class TempoMap {
public:
    static constexpr double kMinBpm = 20.0;
    static constexpr double kMaxBpm = 400.0;

    explicit TempoMap(double bpm = 120.0, int numerator = 4, int denominator = 4);

    // The points and signatures, sorted, each list's first at 0. Setting them tidies what it's given:
    // values clamped (a denominator to the nearest allowed), sorted, entries at the same place as an
    // earlier one dropped, and a copy of the first put at 0 when none is there.
    const std::vector<TempoPoint> &points() const { return points_; }
    const std::vector<Signature> &signatures() const { return signatures_; }
    void setPoints(std::vector<TempoPoint> points);
    void setSignatures(std::vector<Signature> signatures);
    // The opening tempo and signature (the first point's and signature's), the rest left as they are
    double tempo() const { return points_.front().bpm; }
    void setTempo(double bpm);
    const Signature &firstSignature() const { return signatures_.front(); }
    // Whether the map is one tempo and one signature
    bool constant() const { return points_.size() == 1 && signatures_.size() == 1; }

    // Tempo. Before beat 0 the first tempo holds.
    size_t pointAt(double beat) const; // the point in force at `beat`
    double bpmAt(double beat) const;
    double slopeAt(double beat) const; // BPM per beat (0 unless in a ramp)
    double secondsAt(double beat) const;
    double beatAt(double seconds) const;
    double samplesAt(double beat, double sampleRate) const { return secondsAt(beat) * sampleRate; }
    double beatAtSample(double sample, double sampleRate) const { return beatAt(sample / sampleRate); }

    // Metre. Bars before 0 are as long as the first signature's.
    size_t signatureIndexAt(int bar) const;
    const Signature &signatureAtBar(int bar) const { return signatures_[signatureIndexAt(bar)]; }
    const Signature &signatureAt(double beat) const { return signatureAtBar(barAt(beat)); }
    double barStart(int bar) const;
    double barLength(int bar) const { return signatureAtBar(bar).barLength(); }
    int barAt(double beat) const; // the bar `beat` is in (a beat on a bar line is in the bar it starts)
    BarPosition position(double beat) const;
    double nearestBar(double beat) const; // the nearest bar line's beat
    // Calls f(bar, start beat, signature) for every bar that overlaps [from, to)
    template <typename F> void forEachBar(double from, double to, F &&f) const
    {
        for (int bar = barAt(from); barStart(bar) < to; ++bar)
            f(bar, barStart(bar), signatureAtBar(bar));
    }

    bool operator==(const TempoMap &o) const { return points_ == o.points_ && signatures_ == o.signatures_; }
    bool operator!=(const TempoMap &o) const { return !(*this == o); }

private:
    void rebuild();
    double segmentSeconds(size_t i, double beats) const; // seconds `beats` into point i's segment
    double segmentBeats(size_t i, double seconds) const;
    double rampSlope(size_t i) const;

    std::vector<TempoPoint> points_;
    std::vector<Signature> signatures_;
    std::vector<double> pointSeconds_;   // each point's start time
    std::vector<double> signatureBeats_; // each signature's first bar's start beat
};

}
