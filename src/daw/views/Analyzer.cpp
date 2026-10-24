#include "daw/views/Analyzer.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

namespace daw {

using namespace ui;

namespace {

constexpr float kTopDb = 6.0f, kBottomDb = -96.0f;   // the spectrum's range
constexpr float kSideWidth = 280.0f;                  // the right column
constexpr size_t kHistory = 300;                      // 30 s of short-term loudness, every 100 ms
constexpr float kHistoryLow = -60.0f, kHistoryHigh = 0.0f;
constexpr double kLowest = 20.0, kHighest = 20000.0;

std::string printf1(const char *format, double value)
{
    char text[64];
    std::snprintf(text, sizeof(text), format, value);
    return text;
}

// "-14.2" for a level, "-" for silence
std::string lufsText(double lufs)
{
    return std::isfinite(lufs) && lufs > -100.0 ? printf1("%.1f", lufs) : std::string("-");
}

// The note nearest a frequency, with how far off it is: "A4 +3"
std::string noteName(double frequency)
{
    static const char *names[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    const double midi = 69.0 + 12.0 * std::log2(frequency / 440.0);
    const long nearest = std::lround(midi);
    const int cents = static_cast<int>(std::lround((midi - static_cast<double>(nearest)) * 100.0));
    const int index = static_cast<int>(((nearest % 12) + 12) % 12);
    std::string text = std::string(names[index]) + std::to_string(nearest / 12 - 1);
    if (cents != 0)
        text += (cents > 0 ? " +" : " ") + std::to_string(cents);
    return text;
}

std::string frequencyText(double f)
{
    return f >= 1000.0 ? printf1(f >= 10000.0 ? "%.1f kHz" : "%.2f kHz", f / 1000.0) : printf1("%.0f Hz", f);
}

}

AnalyzerView::AnalyzerView(audio::AudioSystem &audio) : audio_(audio)
{
    history_.assign(kHistory, kHistoryLow - 1.0f);
}

AnalyzerView::~AnalyzerView()
{
    // The taps go quiet with the panel
    audio_.masterTap().enabled = false;
    audio_.trackTap().enabled = false;
}

audio::AnalysisTap &AnalyzerView::tap()
{
    return source_ == Source::Master ? audio_.masterTap() : audio_.trackTap();
}

void AnalyzerView::setSource(Source source)
{
    if (source == source_)
        return;
    source_ = source;
    setShowing(showing_);
    reset();
}

void AnalyzerView::setShowing(bool showing)
{
    showing_ = showing;
    audio_.masterTap().enabled = showing && source_ == Source::Master;
    audio_.trackTap().enabled = showing && source_ == Source::Track;
    if (!showing) {
        audio_.masterTap().clear();
        audio_.trackTap().clear();
    }
}

void AnalyzerView::setSize(size_t size)
{
    spectrum_.setSize(size);
}

void AnalyzerView::setHold(bool forever)
{
    spectrum_.setHold(forever ? -1.0f : 3.0f);
    if (!forever)
        spectrum_.reset();
}

void AnalyzerView::reset()
{
    spectrum_.reset();
    stereo_.reset();
    loudness_.reset();
    truePeak_.reset();
    loudnessRate_ = 0.0;
    history_.assign(kHistory, kHistoryLow - 1.0f);
    sinceHistory_ = 0.0;
}

bool AnalyzerView::moving() const
{
    const float floor = audio::SpectrumAnalyzer::kFloorDb + 0.5f;
    for (size_t i = 0; i < audio::SpectrumAnalyzer::kBands; ++i)
        if (spectrum_.bands()[i] > floor || (!holding() && spectrum_.peaks()[i] > floor))
            return true;
    return false;
}

double AnalyzerView::momentary() const
{
    return loudness_ ? loudness_->momentary() : -HUGE_VAL;
}

double AnalyzerView::shortTerm() const
{
    return loudness_ ? loudness_->shortTerm() : -HUGE_VAL;
}

double AnalyzerView::integrated() const
{
    return loudness_ ? loudness_->integrated() : -HUGE_VAL;
}

void AnalyzerView::feed(const float *stereo, size_t frames, double sampleRate)
{
    if (!loudness_ || loudnessRate_ != sampleRate) {
        loudness_ = std::make_unique<audio::master::LoudnessMeter>(2, sampleRate);
        truePeak_ = std::make_unique<audio::master::TruePeakMeter>(2);
        loudnessRate_ = sampleRate;
    }
    spectrum_.push(stereo, frames);
    stereo_.push(stereo, frames, sampleRate);
    loudness_->add(stereo, frames);
    truePeak_->add(stereo, frames);
}

Vec2 AnalyzerView::measure(const Theme &) const
{
    return {480.0f, 220.0f};
}

void AnalyzerView::arrange(const Rect &rect, const Theme &)
{
    rect_ = rect;
    const bool side = rect.w >= 640.0f;
    spectrumRect_ = {rect.x, rect.y, side ? rect.w - kSideWidth - 12.0f : rect.w, rect.h};
    plot_ = {spectrumRect_.x + 40.0f, spectrumRect_.y + 10.0f, std::max(0.0f, spectrumRect_.w - 52.0f),
             std::max(0.0f, spectrumRect_.h - 32.0f)};
    side_ = side ? Rect{rect.x + rect.w - kSideWidth, rect.y, kSideWidth, rect.h} : Rect{};
    if (side) {
        const float square = std::min(kSideWidth * 0.62f, rect.h * 0.48f);
        gonio_ = {side_.x, side_.y, square, square};
        bars_ = {side_.x + square + 10.0f, side_.y, kSideWidth - square - 10.0f, square};
        numbers_ = {side_.x, side_.y + square + 10.0f, kSideWidth, 64.0f};
        const float graphTop = numbers_.y + numbers_.h + 6.0f;
        graph_ = {side_.x, graphTop, kSideWidth, std::max(0.0f, side_.y + side_.h - graphTop)};
    }
}

void AnalyzerView::handleInput(const Input &input)
{
    mouse_ = input.mouse;
    if (!showing_)
        return;
    const double rate = audio_.sampleRate() > 0 ? audio_.sampleRate() : 48000.0;
    // Everything the tap holds, a piece at a time
    buffer_.resize(4096 * 2);
    size_t drained = 0;
    for (;;) {
        const size_t got = tap().read(buffer_.data(), 4096);
        if (got == 0)
            break;
        feed(buffer_.data(), got, rate);
        drained += got;
    }
    spectrum_.update(rate, std::clamp(static_cast<double>(input.dt), 0.0, 0.25));
    // The history and the goniometer's zoom follow the audio that came (not the frames drawn)
    const double seconds = static_cast<double>(drained) / rate;
    if (seconds <= 0.0)
        return;
    // The short-term history, every 100 ms of sound
    sinceHistory_ += seconds;
    while (sinceHistory_ >= 0.1) {
        sinceHistory_ -= 0.1;
        history_.erase(history_.begin());
        const double s = shortTerm();
        history_.push_back(std::isfinite(s) ? static_cast<float>(s) : kHistoryLow - 1.0f);
    }
    // The goniometer zooms to the level, slowly
    float level = 0.0f;
    for (float v : stereo_.points())
        level = std::max(level, std::fabs(v));
    const float target = 1.0f / std::max(level, 0.05f);
    zoom_ += (target - zoom_) * std::min(1.0f, static_cast<float>(seconds) * 3.0f);
}

void AnalyzerView::draw(DrawList &list, const Theme &theme) const
{
    drawSpectrum(list, theme);
    if (side_.w > 0.0f) {
        drawStereo(list, theme);
        drawLoudness(list, theme);
    }
}

void AnalyzerView::drawSpectrum(DrawList &list, const Theme &theme) const
{
    list.add(theme.surface(spectrumRect_, Shape::Soft, Elevation::Inset, 0.6f));
    if (plot_.w <= 0.0f || plot_.h <= 0.0f)
        return;
    auto xOf = [&](double f) {
        return plot_.x + static_cast<float>(std::log(f / kLowest) / std::log(kHighest / kLowest)) * plot_.w;
    };
    auto yOf = [&](float db) { return plot_.y + (kTopDb - db) / (kTopDb - kBottomDb) * plot_.h; };
    // Levels every 12 dB, frequencies at the usual places
    for (int db = 0; db >= static_cast<int>(kBottomDb) + 12; db -= 12) {
        const float y = yOf(static_cast<float>(db));
        list.add(theme.ink({plot_.x, y, plot_.w, 1.0f}, Shape::Rounded, TextTone::Muted, true, db == 0 ? 0.45f : 0.2f));
        list.add(theme.text(std::to_string(db), {spectrumRect_.x + 4.0f, y - 8.0f, 32.0f, 16.0f}, TextStyle::Caption,
                            TextTone::Muted, TextAlign::End));
    }
    const std::pair<double, const char *> marks[] = {{20, "20"},   {50, "50"}, {100, "100"}, {200, "200"}, {500, "500"},
                                                     {1000, "1k"}, {2000, "2k"}, {5000, "5k"}, {10000, "10k"}, {20000, "20k"}};
    for (const auto &[f, label] : marks) {
        const float x = xOf(f);
        list.add(theme.ink({x, plot_.y, 1.0f, plot_.h}, Shape::Rounded, TextTone::Muted, true, 0.2f));
        list.add(theme.text(label, {x - 20.0f, plot_.y + plot_.h + 4.0f, 40.0f, 14.0f}, TextStyle::Caption, TextTone::Muted));
    }

    // The curve over a fill, and the peaks held
    list.pushClip(plot_);
    std::vector<float> values(spectrum_.bands().begin(), spectrum_.bands().end());
    for (float &v : values)
        v = std::clamp(v, kBottomDb, kTopDb);
    Plot fill = theme.accentPlot(plot_, PlotMode::Fill, 0.1f);
    fill.values = values.data();
    fill.count = static_cast<uint32_t>(values.size());
    fill.low = kBottomDb;
    fill.high = kTopDb;
    fill.baseline = kBottomDb;
    list.add(fill);
    std::vector<float> peaks(spectrum_.peaks().begin(), spectrum_.peaks().end());
    for (float &v : peaks)
        v = std::clamp(v, kBottomDb, kTopDb);
    Plot held = theme.inkPlot(plot_, PlotMode::Line, TextTone::Secondary, 0.45f);
    held.values = peaks.data();
    held.count = static_cast<uint32_t>(peaks.size());
    held.low = kBottomDb;
    held.high = kTopDb;
    held.width = 1.0f;
    list.add(held);
    Plot line = theme.accentPlot(plot_, PlotMode::Line, 0.95f);
    line.values = values.data();
    line.count = static_cast<uint32_t>(values.size());
    line.low = kBottomDb;
    line.high = kTopDb;
    line.width = 1.5f;
    list.add(line);
    list.popClip();

    // What is under the mouse: the frequency, its note and the level there
    if (plot_.contains(mouse_)) {
        const double f = kLowest * std::pow(kHighest / kLowest, (mouse_.x - plot_.x) / plot_.w);
        const float db = spectrum_.levelAt(f);
        list.add(theme.ink({mouse_.x, plot_.y, 1.0f, plot_.h}, Shape::Rounded, TextTone::Secondary, true, 0.5f));
        const std::string text = frequencyText(f) + "   " + noteName(f) + "   " +
                                 (db > audio::SpectrumAnalyzer::kFloorDb + 1.0f ? printf1("%.1f dB", db) : std::string("-"));
        const float w = 210.0f;
        const float x = std::min(mouse_.x + 10.0f, plot_.x + plot_.w - w);
        const Rect pill{x, plot_.y + 6.0f, w, 24.0f};
        list.add(theme.surface(pill, Shape::Pill, Elevation::Raised, 0.6f));
        list.add(theme.text(text, pill, TextStyle::Caption, TextTone::Primary));
    }
}

void AnalyzerView::drawStereo(DrawList &list, const Theme &theme) const
{
    // The goniometer: mid up, side across; left on the upper left diagonal, right on the upper right
    list.add(theme.surface(gonio_, Shape::Soft, Elevation::Inset, 0.6f));
    const float cx = gonio_.x + gonio_.w * 0.5f, cy = gonio_.y + gonio_.h * 0.5f;
    const float radius = gonio_.w * 0.45f;
    list.add(theme.ink({cx - 0.5f, gonio_.y + 6.0f, 1.0f, gonio_.h - 12.0f}, Shape::Rounded, TextTone::Muted, true, 0.25f));
    list.add(theme.ink({gonio_.x + 6.0f, cy - 0.5f, gonio_.w - 12.0f, 1.0f}, Shape::Rounded, TextTone::Muted, true, 0.15f));
    for (int k = -20; k <= 20; ++k) { // the L and R diagonals, dotted
        const float t = static_cast<float>(k) / 20.0f * radius * 0.7071f;
        list.add(theme.ink({cx + t - 0.5f, cy - std::fabs(t) - 0.5f, 1.0f, 1.0f}, Shape::Rounded, TextTone::Muted, true, 0.35f));
    }
    list.add(theme.text("L", {gonio_.x + 6.0f, gonio_.y + 4.0f, 16.0f, 14.0f}, TextStyle::Caption, TextTone::Muted, TextAlign::Start));
    list.add(theme.text("R", {gonio_.x + gonio_.w - 22.0f, gonio_.y + 4.0f, 16.0f, 14.0f}, TextStyle::Caption, TextTone::Muted, TextAlign::End));
    list.pushClip(gonio_.inset(2.0f));
    const std::vector<float> points = stereo_.points();
    const float scale = radius * zoom_ * 0.7071f;
    for (size_t i = 0; i + 1 < points.size(); i += 4) {
        const float l = points[i], r = points[i + 1];
        if (l == 0.0f && r == 0.0f)
            continue;
        const float x = cx + (r - l) * scale;
        const float y = cy - (l + r) * scale;
        list.add(theme.accent({x - 1.0f, y - 1.0f, 2.0f, 2.0f}, Shape::Rounded, 0.6f));
    }
    list.popClip();

    // The correlation and the balance: bars from their middle
    auto bar = [&](float y, const std::string &name, float value, const char *low, const char *high) {
        list.add(theme.text(name, {bars_.x, y, bars_.w, 14.0f}, TextStyle::Caption, TextTone::Secondary, TextAlign::Start));
        const Rect groove{bars_.x, y + 18.0f, bars_.w, 10.0f};
        list.add(theme.surface(groove, Shape::Pill, Elevation::Inset, 0.5f));
        const float mid = groove.x + groove.w * 0.5f;
        const float at = mid + std::clamp(value, -1.0f, 1.0f) * (groove.w * 0.5f - 3.0f);
        list.add(theme.accent({std::min(mid, at), groove.y + 2.0f, std::max(2.0f, std::fabs(at - mid)), groove.h - 4.0f}, Shape::Pill, 0.85f));
        list.add(theme.ink({mid - 0.5f, groove.y - 2.0f, 1.0f, groove.h + 4.0f}, Shape::Rounded, TextTone::Muted, true, 0.5f));
        list.add(theme.text(low, {groove.x, groove.y + groove.h + 2.0f, 30.0f, 14.0f}, TextStyle::Caption, TextTone::Muted, TextAlign::Start));
        list.add(theme.text(high, {groove.x + groove.w - 30.0f, groove.y + groove.h + 2.0f, 30.0f, 14.0f}, TextStyle::Caption,
                            TextTone::Muted, TextAlign::End));
    };
    bar(bars_.y + 4.0f, "Correlation " + printf1("%+.2f", stereo_.correlation()), stereo_.correlation(), "-1", "+1");
    bar(bars_.y + 62.0f, "Balance", stereo_.balance(), "L", "R");
    const float width = stereo_.width();
    list.add(theme.text("Width " + (width >= 99.0f ? std::string("all side") : printf1("%.0f%%", 100.0 * std::min(1.0f, width))),
                        {bars_.x, bars_.y + 120.0f, bars_.w, 14.0f}, TextStyle::Caption, TextTone::Secondary, TextAlign::Start));
}

void AnalyzerView::drawLoudness(DrawList &list, const Theme &theme) const
{
    // Momentary, short-term and integrated, large; the range and the true peak under them
    const float third = numbers_.w / 3.0f;
    const std::pair<const char *, double> values[] = {{"Momentary", momentary()}, {"Short-term", shortTerm()}, {"Integrated", integrated()}};
    for (int i = 0; i < 3; ++i) {
        const Rect cell{numbers_.x + third * static_cast<float>(i), numbers_.y, third, numbers_.h};
        list.add(theme.text(values[i].first, {cell.x, cell.y, cell.w, 14.0f}, TextStyle::Caption, TextTone::Secondary));
        list.add(theme.text(lufsText(values[i].second), {cell.x, cell.y + 16.0f, cell.w, 28.0f}, TextStyle::Title,
                            i == 2 ? TextTone::Accent : TextTone::Primary));
    }
    const std::string range = loudness_ ? printf1("LRA %.1f LU", loudness_->range()) : std::string("LRA -");
    const double peak = truePeak_ ? truePeak_->peakDb() : -HUGE_VAL;
    const std::string tp = std::isfinite(peak) && peak > -100.0 ? printf1("True peak %.1f dBTP", peak) : std::string("True peak -");
    list.add(theme.text("LUFS   " + range + "   " + tp, {numbers_.x, numbers_.y + 46.0f, numbers_.w, 14.0f}, TextStyle::Caption,
                        TextTone::Muted));

    // 30 s of short-term loudness
    if (graph_.h < 30.0f)
        return;
    list.add(theme.surface(graph_, Shape::Soft, Elevation::Inset, 0.5f));
    const Rect inner = graph_.inset(6.0f);
    // Lines every 12 dB, labelled where there is room for them
    const float spacing = inner.h * 12.0f / (kHistoryHigh - kHistoryLow);
    for (float db : {-12.0f, -24.0f, -36.0f, -48.0f}) {
        const float y = inner.y + (kHistoryHigh - db) / (kHistoryHigh - kHistoryLow) * inner.h;
        list.add(theme.ink({inner.x, y, inner.w, 1.0f}, Shape::Rounded, TextTone::Muted, true, 0.18f));
        if (spacing >= 18.0f || db == -24.0f)
            list.add(theme.text(std::to_string(static_cast<int>(db)), {inner.x, y - 14.0f, 30.0f, 12.0f}, TextStyle::Caption,
                                TextTone::Muted, TextAlign::Start));
    }
    list.pushClip(inner);
    Plot line = theme.accentPlot(inner, PlotMode::Line, 0.9f);
    line.values = history_.data();
    line.count = static_cast<uint32_t>(history_.size());
    line.low = kHistoryLow;
    line.high = kHistoryHigh;
    line.width = 1.5f;
    list.add(line);
    list.popClip();
    list.add(theme.text("Short-term, 30 s", {inner.x, inner.y + inner.h - 14.0f, inner.w, 14.0f}, TextStyle::Caption,
                        TextTone::Muted, TextAlign::End));
}

}
