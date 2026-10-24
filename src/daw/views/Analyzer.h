#pragma once

#include <memory>
#include <string>
#include <vector>

#include "audio/Analysis.h"
#include "audio/AudioSystem.h"
#include "audio/Mastering.h"
#include "ui/UI.h"

namespace daw {

// The Analyzer panel (docs/design/analyzer-plan.md): the spectrum, the stereo picture and the
// loudness of the master, or of the selected track, as it plays. It drains its tap of the AudioSystem
// each frame in handleInput (the taps are on only while it shows: setShowing).
// - The spectrum on the left: levels on log-spaced bands from 20 Hz to 20 kHz in an inset well, the
//   curve filled under its line, the held peaks as a faint line; the mouse reads the frequency, the
//   nearest note and the level.
// - On the right a goniometer (the newest frames as dots, mid up and side across, zooming to the
//   level), the correlation and the balance as bars, and the loudness: momentary, short-term and
//   integrated (since Reset or the source changed), the range, the true peak, and 30 s of short-term
//   loudness as a line.
class AnalyzerView : public ui::Component {
public:
    enum class Source { Master, Track };

    explicit AnalyzerView(audio::AudioSystem &audio);
    ~AnalyzerView() override;

    void setSource(Source source);
    Source source() const { return source_; }
    // Whether the panel shows: the tap it reads is on only then
    void setShowing(bool showing);
    // Settings: the FFT's size, the tilt (dB per octave), peaks held until reset
    void setSize(size_t size);
    size_t size() const { return spectrum_.size(); }
    void setTilt(float dbPerOctave) { spectrum_.setTilt(dbPerOctave); }
    float tilt() const { return spectrum_.tilt(); }
    void setHold(bool forever);
    bool holding() const { return spectrum_.hold() < 0.0f; }
    // Starts the loudness and the peaks again
    void reset();
    // Whether it still changes with nothing coming in (a band or a peak still falling)
    bool moving() const;

    // What is measured now, for tests and the header
    const audio::SpectrumAnalyzer &spectrum() const { return spectrum_; }
    const audio::StereoMeter &stereo() const { return stereo_; }
    double momentary() const;
    double shortTerm() const;
    double integrated() const;

    ui::Vec2 measure(const ui::Theme &theme) const override;
    void arrange(const ui::Rect &rect, const ui::Theme &theme) override;
    void handleInput(const ui::Input &input) override;
    void draw(ui::DrawList &list, const ui::Theme &theme) const override;

    // Feeds audio as the tap would (tests and the render suite's scene)
    void feed(const float *stereo, size_t frames, double sampleRate);

private:
    audio::AnalysisTap &tap();
    void drawSpectrum(ui::DrawList &list, const ui::Theme &theme) const;
    void drawStereo(ui::DrawList &list, const ui::Theme &theme) const;
    void drawLoudness(ui::DrawList &list, const ui::Theme &theme) const;

    audio::AudioSystem &audio_;
    Source source_ = Source::Master;
    bool showing_ = false;
    audio::SpectrumAnalyzer spectrum_;
    audio::StereoMeter stereo_;
    std::unique_ptr<audio::master::LoudnessMeter> loudness_;
    std::unique_ptr<audio::master::TruePeakMeter> truePeak_;
    double loudnessRate_ = 0.0;
    std::vector<float> history_; // short-term loudness every 100 ms, the newest last (30 s)
    double sinceHistory_ = 0.0;
    std::vector<float> buffer_;  // what was drained this frame
    float zoom_ = 1.0f;          // the goniometer's
    ui::Rect spectrumRect_, plot_, side_, gonio_, bars_, numbers_, graph_;
    ui::Vec2 mouse_{-1e9f, -1e9f};
};

}
