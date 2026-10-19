#include "SynthView.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <memory>

#include "Format.h"
#include "SynthUserPresets.h"
#include "Widgets.h"
#include "audio/Synth.h"

namespace daw {

using namespace ui;

namespace {

namespace sp = audio::synth;

constexpr float kKnob = 40.0f;       // the knobs' diameter
constexpr float kDisplay = 64.0f;    // the envelopes' display height
constexpr float kOscDisplay = 52.0f; // the oscillators' wave display
constexpr float kLfoDisplay = 56.0f; // the LFOs' wave display
constexpr float kSubDisplay = 56.0f; // the sub and noise's display
constexpr double kPlateau = 0.6;     // the envelope's sustain, as a width in the envelope's scale
constexpr double kSpan = 6.0;        // the least width the envelope's scale fills
constexpr double kFreqMin = 20.0, kFreqMax = 20000.0;
constexpr double kDbMin = -36.0, kDbMax = 18.0;

// A row of controls: packed tight, a little apart
class SynthRow : public Container {
public:
    SynthRow() : Container(Direction::Row) { setJustify(Justify::Start); }
    float gap(const Theme &) const override { return 6.0f; }
    float padding(const Theme &) const override { return 0.0f; }
};

// A column of a card's content: its displays and rows close together
class SynthColumn : public Container {
public:
    SynthColumn() : Container(Direction::Column) {}
    float gap(const Theme &) const override { return 6.0f; }
    float padding(const Theme &) const override { return 0.0f; }
};

// A parameter of the synth by its id (ids are all known)
const audio::DeviceParam &paramOf(uint32_t id)
{
    static const std::map<uint32_t, audio::DeviceParam> params = [] {
        std::map<uint32_t, audio::DeviceParam> m;
        for (const audio::DeviceParam &p : audio::synth::synthParams())
            m.emplace(p.id, p);
        return m;
    }();
    return params.at(id);
}

// Every parameter's value: the defaults, then `overlay` on top (known ids only), sorted by id
plugin::ParamValues fullValues(const plugin::ParamValues &overlay)
{
    std::map<uint32_t, double> values;
    for (const audio::DeviceParam &p : audio::synth::synthParams())
        values[p.id] = p.def;
    for (const auto &[id, value] : overlay)
        if (values.count(id))
            values[id] = value;
    return plugin::ParamValues(values.begin(), values.end());
}

// The envelope's time scale: logarithmic, so a 2 ms attack and a 10 s release both show. The
// scale of a time, and a time of a scale value
double timeScale(double ms)
{
    return std::log10(1.0 + std::max(0.0, ms) / 2.0);
}

double msFromScale(double scale)
{
    return 2.0 * (std::pow(10.0, std::max(0.0, scale)) - 1.0);
}

// The envelope's times (ms) and sustain (0-1)
struct Envelope {
    double attack, decay, release, sustain;
};

Envelope readEnvelope(const DeviceAccess &access, uint32_t base)
{
    return {paramValue(access, paramOf(base)), paramValue(access, paramOf(base + 1)), paramValue(access, paramOf(base + 3)),
            paramValue(access, paramOf(base + 2)) / 100.0};
}

// Where each stage ends, as fractions of the width. The scale fills at least kSpan, so a short
// envelope leaves its right end free, and a long one fills the width.
struct Marks {
    double attack, decay, sustain, release;
};

Marks marksOf(const Envelope &e)
{
    const double ga = timeScale(e.attack), gd = timeScale(e.decay), gr = timeScale(e.release);
    const double span = std::max(ga + gd + kPlateau + gr, kSpan);
    return {ga / span, (ga + gd) / span, (ga + gd + kPlateau) / span, (ga + gd + kPlateau + gr) / span};
}

// The envelope's height (0-1) at a fraction of the width, or -1 past its end
double levelAt(const Marks &m, double sustain, double x)
{
    if (x > m.release)
        return -1.0;
    if (x <= m.attack)
        return m.attack > 0.0 ? x / m.attack : 1.0;
    if (x <= m.decay)
        return m.decay - m.attack > 1e-9 ? sustain + (1.0 - sustain) * std::exp(-5.0 * (x - m.attack) / (m.decay - m.attack)) : sustain;
    if (x <= m.sustain)
        return sustain;
    return m.release - m.sustain > 1e-9 ? sustain * std::exp(-5.0 * (x - m.sustain) / (m.release - m.sustain)) : 0.0;
}

// A raised knob with an accent core, as the piano roll's points are drawn
void knob(DrawList &list, const Theme &theme, Vec2 at, float diameter, bool hot)
{
    const Rect r{at.x - diameter * 0.5f, at.y - diameter * 0.5f, diameter, diameter};
    list.add(theme.surface(r, Shape::Circle, Elevation::Raised, hot ? 0.7f : 0.4f, Finish::Convex));
    list.add(theme.accent(r.inset(2.5f), Shape::Circle, hot ? 1.0f : 0.8f));
}

// One cycle of a wave across the well's width, from its middle, as columns; a curve of accent
void drawCycle(DrawList &list, const Theme &theme, const Rect &area, const std::vector<float> &cycle, float opacity)
{
    const float middle = area.y + area.h * 0.5f;
    for (size_t c = 0; c < cycle.size(); ++c) {
        const float x = area.x + 2.0f * static_cast<float>(c);
        const float y = middle - cycle[c] * area.h * 0.45f;
        list.add(theme.accent({x, std::min(y, middle), 2.0f, std::max(1.0f, std::fabs(middle - y))}, Shape::Rounded, 0.13f * opacity));
        list.add(theme.accent({x, y - 1.0f, 2.0f, 2.0f}, Shape::Rounded, 0.9f * opacity));
    }
}

// One cycle of an oscillator's wave, in a well
class WaveDisplay : public Component {
public:
    WaveDisplay(DeviceAccess access, uint32_t osc) : access_(std::move(access)), osc_(osc) {}

    Vec2 measure(const Theme &) const override { return {240.0f, kOscDisplay}; }

    void draw(DrawList &list, const Theme &theme) const override
    {
        list.add(theme.surface(rect_, Shape::Soft, Elevation::Inset, 0.6f));
        const Rect area = rect_.inset(8.0f);
        const int columns = std::max(1, static_cast<int>(area.w / 2.0f));
        std::vector<float> cycle(static_cast<size_t>(columns));
        const int wave = static_cast<int>(std::lround(paramValue(access_, paramOf(osc_ + sp::kOscWave))));
        const float shape = static_cast<float>(paramValue(access_, paramOf(osc_ + sp::kOscShape)) / 100.0);
        sp::previewCycle(wave, shape, cycle.data(), columns);
        // An oscillator at no level shows its wave faintly
        const float opacity = paramValue(access_, paramOf(osc_ + sp::kOscLevel)) > 0.0 ? 1.0f : 0.3f;
        list.add(theme.ink({area.x, area.y + area.h * 0.5f, area.w, 1.0f}, Shape::Rounded, TextTone::Muted, true, 0.3f));
        drawCycle(list, theme, area, cycle, opacity);
    }

private:
    DeviceAccess access_;
    uint32_t osc_;
};

// The sub oscillator's wave (sine or square, an octave or two under) and the noise's colour as a
// tilt: flat for white noise, falling steeply for dark
class SubDisplay : public Component {
public:
    explicit SubDisplay(DeviceAccess access) : access_(std::move(access)) {}

    Vec2 measure(const Theme &) const override { return {240.0f, kSubDisplay}; }

    void draw(DrawList &list, const Theme &theme) const override
    {
        list.add(theme.surface(rect_, Shape::Soft, Elevation::Inset, 0.6f));
        const Rect area = rect_.inset(8.0f);
        const int columns = std::max(1, static_cast<int>(area.w / 2.0f));
        std::vector<float> cycle(static_cast<size_t>(columns));
        const bool square = paramValue(access_, paramOf(sp::kSubWave)) >= 0.5;
        sp::previewCycle(square ? sp::kSquare : sp::kSine, 0.5f, cycle.data(), columns);
        const bool subOn = paramValue(access_, paramOf(sp::kSubLevel)) > 0.0;
        const float middle = area.y + area.h * 0.5f;
        list.add(theme.ink({area.x, middle, area.w, 1.0f}, Shape::Rounded, TextTone::Muted, true, 0.3f));
        drawCycle(list, theme, area, cycle, subOn ? 1.0f : 0.3f);
        // The noise: a line from its left to its right, by colour and level
        const double colour = paramValue(access_, paramOf(sp::kNoiseColour)) / 100.0;
        const double level = paramValue(access_, paramOf(sp::kNoiseLevel)) / 100.0;
        const float noiseOpacity = level > 0.0 ? 0.25f + 0.5f * static_cast<float>(level) : 0.12f;
        for (float x = area.x; x < area.x + area.w; x += 2.0f) {
            const double t = static_cast<double>((x - area.x) / area.w);
            const float y = area.y + area.h * static_cast<float>(0.3 + 0.4 * (colour * t + (1.0 - colour) * 0.5));
            list.add(theme.ink({x, y - 1.0f, 2.0f, 2.0f}, Shape::Rounded, TextTone::Secondary, true, noiseOpacity));
        }
    }

private:
    DeviceAccess access_;
};

// The filter's magnitude response over 20 Hz to 20 kHz, from the analogue prototype of the TPT
// state-variable filter: with u = f / cutoff and k = 2 - 1.96 res, the low-pass is 1 / |1 - u^2 +
// j k u|, the others share that denominator (LP24 is two of them, with k from 0.7 of the
// resonance).
double filterDb(int type, double hz, double cutoff, double res)
{
    const double u = hz / cutoff;
    const double k = 2.0 - 1.96 * res;
    const double d = std::hypot(1.0 - u * u, k * u);
    double mag = 1.0;
    switch (type) {
    case 0: mag = 1.0 / d; break;                                                   // low-pass 12
    case 1: {                                                                        // low-pass 24
        const double k2 = 2.0 - 1.96 * res * 0.7;
        const double d2 = std::hypot(1.0 - u * u, k2 * u);
        mag = 1.0 / (d2 * d2);
        break;
    }
    case 2: mag = u * u / d; break;                                                 // high-pass 12
    case 3: mag = k * u / d; break;                                                 // band-pass
    default: mag = std::fabs(1.0 - u * u) / d; break;                               // notch
    }
    return std::clamp(20.0 * std::log10(std::max(mag, 1e-6)), kDbMin, kDbMax);
}

class FilterCurve : public Component {
public:
    explicit FilterCurve(DeviceAccess access) : access_(std::move(access)) {}

    Vec2 measure(const Theme &) const override { return {280.0f, 80.0f}; }

    void handleInput(const Input &input) override
    {
        const Rect area = rect_.inset(8.0f);
        const bool inside = rect_.contains(input.mouse);
        if (input.pressed && inside) {
            // A second press within a third of a second resets the cutoff and resonance
            if (input.time - lastClick_ < 0.35) {
                setParamValue(access_, paramOf(sp::kCutoff), paramOf(sp::kCutoff).def);
                setParamValue(access_, paramOf(sp::kResonance), paramOf(sp::kResonance).def);
                lastClick_ = -1e9;
                dragging_ = false;
            } else {
                lastClick_ = input.time;
                dragging_ = true;
            }
        }
        if (!input.down)
            dragging_ = false;
        if (dragging_) {
            const double t = std::clamp((input.mouse.x - area.x) / std::max(1.0f, area.w), 0.0f, 1.0f);
            const double hz = kFreqMin * std::pow(kFreqMax / kFreqMin, t);
            const double res = std::clamp((area.y + area.h - input.mouse.y) / std::max(1.0f, area.h), 0.0f, 1.0f);
            setParamValue(access_, paramOf(sp::kCutoff), hz);
            setParamValue(access_, paramOf(sp::kResonance), res * 100.0);
        }
        if (inside) {
            const double cutoff = paramValue(access_, paramOf(sp::kCutoff));
            const float x = freqX(area, cutoff);
            input.setCursor(dragging_ ? engine::Cursor::Move
                                      : std::fabs(input.mouse.x - x) < 6.0f ? engine::Cursor::ResizeX : engine::Cursor::Crosshair);
        }
    }

    void draw(DrawList &list, const Theme &theme) const override
    {
        list.add(theme.surface(rect_, Shape::Soft, Elevation::Inset, 0.6f));
        const Rect area = rect_.inset(8.0f);
        const int type = static_cast<int>(std::lround(paramValue(access_, paramOf(sp::kFilterType))));
        const double cutoff = paramValue(access_, paramOf(sp::kCutoff));
        const double res = paramValue(access_, paramOf(sp::kResonance)) / 100.0;
        const float zero = dbY(area, 0.0);
        list.add(theme.ink({area.x, zero, area.w, 1.0f}, Shape::Rounded, TextTone::Muted, true, 0.3f));
        // The response: a soft fill down to the bottom, and a bright line three pixels thick
        const float bottom = area.y + area.h;
        for (float x = area.x; x < area.x + area.w; x += 2.0f) {
            const double hz = kFreqMin * std::pow(kFreqMax / kFreqMin, static_cast<double>((x - area.x) / area.w));
            const float y = dbY(area, filterDb(type, hz, cutoff, res));
            list.add(theme.accent({x, y, 2.0f, std::max(1.0f, bottom - y)}, Shape::Rounded, 0.06f));
            list.add(theme.accent({x, y - 1.5f, 2.0f, 3.0f}, Shape::Rounded, 0.95f));
        }
        // The cutoff's line, and its raised handle on the curve (the drag target)
        const float cx = freqX(area, cutoff);
        list.add(theme.accentLine({cx + 0.5f, area.y}, {cx + 0.5f, area.y + area.h}, 1.0f, 0.3f));
        knob(list, theme, {cx, dbY(area, filterDb(type, cutoff, cutoff, res))}, 12.0f, dragging_);
    }

private:
    static float freqX(const Rect &area, double hz)
    {
        return area.x + area.w * static_cast<float>(std::log(hz / kFreqMin) / std::log(kFreqMax / kFreqMin));
    }
    static float dbY(const Rect &area, double db)
    {
        return area.y + area.h * static_cast<float>((kDbMax - db) / (kDbMax - kDbMin));
    }

    DeviceAccess access_;
    bool dragging_ = false;
    double lastClick_ = -1e9;
};

// The envelope's shape: attack, decay to the sustain, a plateau, then the release. Three knobs
// move its stages; the readout says what is moving while it is.
class EnvelopeDisplay : public Component {
public:
    EnvelopeDisplay(DeviceAccess access, uint32_t base) : access_(std::move(access)), base_(base) {}

    Vec2 measure(const Theme &) const override { return {280.0f, kDisplay}; }

    void handleInput(const Input &input) override
    {
        const Rect area = rect_.inset(8.0f);
        const float bottom = area.y + area.h;
        const Envelope e = readEnvelope(access_, base_);
        const Marks m = marksOf(e);
        const Vec2 handles[3] = {{area.x + static_cast<float>(m.attack) * area.w, area.y},
                                 {area.x + static_cast<float>(m.decay) * area.w, bottom - static_cast<float>(e.sustain) * area.h},
                                 {area.x + static_cast<float>(m.release) * area.w, bottom}};
        if (input.pressed && rect_.contains(input.mouse)) {
            // The nearest handle takes the drag
            drag_ = 0;
            float best = 1e9f;
            for (int i = 0; i < 3; ++i) {
                const float d = std::hypot(handles[i].x - input.mouse.x, handles[i].y - input.mouse.y);
                if (d < best) {
                    best = d;
                    drag_ = i;
                }
            }
        }
        if (!input.down)
            drag_ = -1;
        if (drag_ >= 0) {
            // The scale of the whole envelope, as it is now
            const double ga = timeScale(e.attack), gd = timeScale(e.decay), gr = timeScale(e.release);
            const double span = std::max(ga + gd + kPlateau + gr, kSpan);
            const double f = std::clamp((input.mouse.x - area.x) / std::max(1.0f, area.w), 0.0f, 1.0f);
            const double s = std::clamp((bottom - input.mouse.y) / std::max(1.0f, area.h), 0.0f, 1.0f);
            if (drag_ == 0) {
                setParamValue(access_, paramOf(base_), msFromScale(f * span));
            } else if (drag_ == 1) {
                setParamValue(access_, paramOf(base_ + 1), msFromScale(std::max(0.0, f * span - ga)));
                setParamValue(access_, paramOf(base_ + 2), s * 100.0);
            } else {
                setParamValue(access_, paramOf(base_ + 3), msFromScale(std::max(0.0, f * span - (ga + gd + kPlateau))));
            }
        }
        if (rect_.contains(input.mouse))
            input.setCursor(engine::Cursor::Hand);
    }

    void draw(DrawList &list, const Theme &theme) const override
    {
        list.add(theme.surface(rect_, Shape::Soft, Elevation::Inset, 0.6f));
        const Rect area = rect_.inset(8.0f);
        const float bottom = area.y + area.h;
        const Envelope e = readEnvelope(access_, base_);
        const Marks m = marksOf(e);
        std::vector<Vec2> points;
        points.reserve(static_cast<size_t>(area.w / 2.0f) + 1);
        for (float x = area.x; x < area.x + area.w; x += 2.0f) {
            const double level = levelAt(m, e.sustain, static_cast<double>((x - area.x) / area.w));
            if (level < 0.0)
                break;
            const float y = bottom - static_cast<float>(level) * area.h;
            list.add(theme.accent({x, y, 2.0f, std::max(1.0f, bottom - y)}, Shape::Rounded, 0.12f));
            points.push_back(Vec2{x + 1.0f, y});
        }
        list.addPolyline(points.data(), points.size(), theme.accentLine({}, {}, 2.0f, 0.9f));
        const Vec2 handles[3] = {{area.x + static_cast<float>(m.attack) * area.w, area.y},
                                 {area.x + static_cast<float>(m.decay) * area.w, bottom - static_cast<float>(e.sustain) * area.h},
                                 {area.x + static_cast<float>(m.release) * area.w, bottom}};
        for (int i = 0; i < 3; ++i)
            knob(list, theme, handles[i], 10.0f, drag_ == i);
        if (drag_ >= 0) {
            std::string text;
            if (drag_ == 0)
                text = "A " + format("%.1f", e.attack) + " ms";
            else if (drag_ == 1)
                text = "D " + format("%.1f", e.decay) + " ms, S " + format("%.0f", e.sustain * 100.0) + "%";
            else
                text = "R " + format("%.1f", e.release) + " ms";
            list.add(theme.text(text, {rect_.x + 8.0f, rect_.y + 2.0f, rect_.w - 16.0f, 16.0f}, TextStyle::Caption, TextTone::Accent,
                                TextAlign::Start));
        }
    }

private:
    DeviceAccess access_;
    uint32_t base_;
    int drag_ = -1;
};

// The LFO's wave over two cycles, dimmed when no modulation slot uses it. Random is a fixed
// staircase of eight values; Smooth random glides between them.
class LfoDisplay : public Component {
public:
    LfoDisplay(DeviceAccess access, int lfo) : access_(std::move(access)), lfo_(lfo) {}

    Vec2 measure(const Theme &) const override { return {240.0f, kLfoDisplay}; }

    void draw(DrawList &list, const Theme &theme) const override
    {
        list.add(theme.surface(rect_, Shape::Soft, Elevation::Inset, 0.6f));
        const Rect area = rect_.inset(8.0f);
        bool used = false;
        for (uint32_t slot = 0; slot < 6; ++slot)
            used = used || std::lround(paramValue(access_, paramOf(sp::kModSlotBase + 3 * slot))) == 1 + lfo_;
        const float opacity = used ? 1.0f : 0.3f;
        const int wave = static_cast<int>(std::lround(paramValue(access_, paramOf(sp::kLfoBase[lfo_] + sp::kLfoWave))));
        const float middle = area.y + area.h * 0.5f;
        list.add(theme.ink({area.x, middle, area.w, 1.0f}, Shape::Rounded, TextTone::Muted, true, 0.3f));
        for (float x = area.x; x < area.x + area.w; x += 2.0f) {
            const double t = 2.0 * static_cast<double>((x - area.x) / area.w); // two cycles
            const float y = middle - static_cast<float>(value(wave, t)) * area.h * 0.45f;
            list.add(theme.accent({x, std::min(y, middle), 2.0f, std::max(1.0f, std::fabs(middle - y))}, Shape::Rounded, 0.13f * opacity));
            list.add(theme.accent({x, y - 1.0f, 2.0f, 2.0f}, Shape::Rounded, 0.9f * opacity));
        }
    }

private:
    // The value (-1..1) at t cycles
    static double value(int wave, double t)
    {
        static const double steps[8] = {0.6, -0.8, 0.2, 0.9, -0.3, -0.9, 0.7, -0.1};
        const double phase = t - std::floor(t);
        switch (wave) {
        case 1: return phase < 0.5 ? -1.0 + 4.0 * phase : 3.0 - 4.0 * phase;   // triangle
        case 2: return 2.0 * phase - 1.0;                                       // saw
        case 3: return phase < 0.5 ? 1.0 : -1.0;                                // square
        case 4: return steps[static_cast<int>(std::floor(t * 4.0)) % 8];        // random
        case 5: {                                                               // smooth random
            const double at = t * 4.0;
            const int i = static_cast<int>(std::floor(at)) % 8;
            const double f = at - std::floor(at);
            return steps[i] + (steps[(i + 1) % 8] - steps[i]) * f;
        }
        default: return std::sin(2.0 * 3.14159265358979323846 * phase);          // sine
        }
    }

    DeviceAccess access_;
    int lfo_;
};

}

SynthView::SynthView(Context &context, DeviceAccess access) : Container(Direction::Column), context_(context), access_(std::move(access))
{
    setAlign(Align::Stretch);
    presetBar(*this);

    auto &scroll = add<ScrollView>();
    scroll.setGrow(1.0f);
    Container &rows = scroll.content();
    rows.setAlign(Align::Stretch);

    // Row 1: the oscillators
    auto &row1 = rows.add<SynthRow>();
    row1.setAlign(Align::Stretch);
    oscillator(row1, 0);
    oscillator(row1, 1);

    // Row 2: the filter and its envelopes
    auto &row2 = rows.add<SynthRow>();
    row2.setAlign(Align::Stretch);
    filterCard(row2);
    envelopeCard(row2, sp::kFilterAttack, "Filter envelope", false);
    envelopeCard(row2, sp::kAmpAttack, "Amp envelope", true);

    // Row 3: the LFOs, and the sub and noise as a narrower card
    auto &row3 = rows.add<SynthRow>();
    row3.setAlign(Align::Stretch);
    lfoCard(row3, 0);
    lfoCard(row3, 1);
    subAndNoise(row3);

    // Row 4: the modulation matrix (two columns of three slots) and the voice
    auto &row4 = rows.add<SynthRow>();
    row4.setAlign(Align::Stretch);
    modulationCard(row4);
    voiceCard(row4);

    refreshPresets();
}

void SynthView::handleInput(const Input &input)
{
    // The preset list follows the device's name, values and the user's presets, a frame late at most
    if (presetsSignature() != presetsBuilt_ && !presetsPosted_) {
        presetsPosted_ = true;
        context_.post([this] {
            presetsPosted_ = false;
            refreshPresets();
            presetsBuilt_ = presetsSignature();
        });
    }
    // The LFO's rate column follows its Sync
    for (int lfo = 0; lfo < 2; ++lfo) {
        const int sync = paramValue(access_, paramOf(sp::kLfoBase[lfo] + sp::kLfoSync)) >= 0.5 ? 1 : 0;
        if (sync != rateBuilt_[lfo] && !ratePosted_[lfo]) {
            ratePosted_[lfo] = true;
            context_.post([this, lfo] {
                ratePosted_[lfo] = false;
                rebuildRate(lfo);
            });
        }
    }
    Container::handleInput(input);
}

Container &SynthView::card(Container &parent, const std::string &title)
{
    auto &panel = parent.add<Panel>(Direction::Column);
    panel.setAlign(Align::Stretch);
    panel.setGrow(1.0f);
    panel.add<Label>(title, TextStyle::Caption, TextTone::Accent);
    auto &body = panel.add<SynthColumn>();
    body.setAlign(Align::Stretch);
    return body;
}

void SynthView::presetBar(Container &parent)
{
    auto &bar = parent.add<Container>(Direction::Row);
    bar.setAlign(Align::Center);
    bar.add<Label>("Preset", TextStyle::Caption, TextTone::Muted);
    presetDrop_ = &bar.add<Dropdown>(context_, std::vector<std::string>{}, 0, [this](int i) { choosePreset(i); });
    presetDrop_->setGrow(1.0f);
    bar.add<Button>([this] { stepPreset(-1); }).setLabel("Previous").setVariant(ButtonVariant::Flat).setSize(Size::Small);
    bar.add<Button>([this] { stepPreset(1); }).setLabel("Next").setVariant(ButtonVariant::Flat).setSize(Size::Small);
    bar.add<Button>([this] { context_.post([this] { openSaveDialog(); }); })
        .setLabel("Save preset...")
        .setVariant(ButtonVariant::Flat)
        .setSize(Size::Small);
    bar.add<Button>([this] { initPreset(); }).setLabel("Init").setVariant(ButtonVariant::Flat).setSize(Size::Small);
    // The dropdown shows the device's name (and whether it is edited), whatever the values
    Dropdown *drop = presetDrop_;
    bar.add<FrameHook>([this, drop](const Input &) {
        const int index = selectedPreset();
        if (static_cast<size_t>(index) < drop->options().size())
            drop->setSelected(index);
    });
}

void SynthView::oscillator(Container &parent, int osc)
{
    const uint32_t b = sp::kOscBase[osc];
    auto &body = card(parent, osc == 0 ? "Osc 1" : "Osc 2");
    body.add<WaveDisplay>(access_, b);
    auto &first = body.add<SynthRow>();
    first.setAlign(Align::Start);
    const uint32_t top[] = {sp::kOscWave, sp::kOscShape, sp::kOscLevel, sp::kOscDetune, sp::kOscUnison, sp::kOscSpread};
    for (uint32_t id : top)
        addParamControl(context_, first, access_, paramOf(b + id), kKnob);
    auto &second = body.add<SynthRow>();
    second.setAlign(Align::Start);
    const uint32_t tuning[] = {sp::kOscOctave, sp::kOscSemitones, sp::kOscFine};
    for (uint32_t id : tuning)
        addParamControl(context_, second, access_, paramOf(b + id), kKnob);
}

void SynthView::subAndNoise(Container &parent)
{
    // Narrow: the sub's two choices and the noise's two knobs in rows of two and three
    auto &body = card(parent, "Sub & noise");
    body.add<SubDisplay>(access_);
    auto &first = body.add<SynthRow>();
    first.setAlign(Align::Start);
    addParamControl(context_, first, access_, paramOf(sp::kSubLevel), kKnob);
    addParamControl(context_, first, access_, paramOf(sp::kSubOctave), kKnob);
    auto &second = body.add<SynthRow>();
    second.setAlign(Align::Start);
    addParamControl(context_, second, access_, paramOf(sp::kSubWave), kKnob);
    addParamControl(context_, second, access_, paramOf(sp::kNoiseLevel), kKnob);
    addParamControl(context_, second, access_, paramOf(sp::kNoiseColour), kKnob);
}

void SynthView::filterCard(Container &parent)
{
    auto &body = card(parent, "Filter");
    body.add<FilterCurve>(access_);
    auto &first = body.add<SynthRow>();
    first.setAlign(Align::Start);
    const uint32_t top[] = {sp::kFilterType, sp::kCutoff, sp::kResonance, sp::kDrive};
    for (uint32_t id : top)
        addParamControl(context_, first, access_, paramOf(id), kKnob);
    auto &second = body.add<SynthRow>();
    second.setAlign(Align::Start);
    const uint32_t bottom[] = {sp::kEnvAmount, sp::kKeyTracking, sp::kFilterVelocity};
    for (uint32_t id : bottom)
        addParamControl(context_, second, access_, paramOf(id), kKnob);
}

void SynthView::envelopeCard(Container &parent, uint32_t base, const std::string &title, bool velocity)
{
    auto &body = card(parent, title);
    body.add<EnvelopeDisplay>(access_, base);
    auto &line = body.add<SynthRow>();
    line.setAlign(Align::Start);
    for (uint32_t offset = 0; offset < 4; ++offset)
        addParamControl(context_, line, access_, paramOf(base + offset), kKnob);
    if (velocity)
        addParamControl(context_, line, access_, paramOf(sp::kAmpVelocity), kKnob);
}

void SynthView::lfoCard(Container &parent, int lfo)
{
    const uint32_t b = sp::kLfoBase[lfo];
    auto &body = card(parent, lfo == 0 ? "LFO 1" : "LFO 2");
    body.add<LfoDisplay>(access_, lfo);
    auto &line = body.add<SynthRow>();
    line.setAlign(Align::Start);
    addParamControl(context_, line, access_, paramOf(b + sp::kLfoWave), kKnob);
    // Rate or Sync rate, whichever the Sync switch leaves in use
    rate_[lfo] = &line.add<ParamCell>(Direction::Column);
    rate_[lfo]->setAlign(Align::Center);
    rebuildRate(lfo);
    addParamControl(context_, line, access_, paramOf(b + sp::kLfoSync), kKnob);
    addParamControl(context_, line, access_, paramOf(b + sp::kLfoRetrigger), kKnob);
    addParamControl(context_, line, access_, paramOf(b + sp::kLfoFade), kKnob);
}

void SynthView::rebuildRate(int lfo)
{
    const uint32_t b = sp::kLfoBase[lfo];
    const bool sync = paramValue(access_, paramOf(b + sp::kLfoSync)) >= 0.5;
    rate_[lfo]->clear();
    addParamControl(context_, *rate_[lfo], access_, paramOf(b + (sync ? sp::kLfoSyncRate : sp::kLfoRate)), kKnob);
    rateBuilt_[lfo] = sync ? 1 : 0;
}

void SynthView::modulationCard(Container &parent)
{
    auto &body = card(parent, "Modulation");
    // Slots 1-3 in the left column, 4-6 in the right; each slot is one line: number, source, an
    // arrow, target, and a small amount with its value beside it
    auto &columns = body.add<SynthRow>();
    columns.setAlign(Align::Start);
    for (uint32_t column = 0; column < 2; ++column) {
        auto &lines = columns.add<SynthColumn>();
        lines.setAlign(Align::Start);
        for (uint32_t k = 0; k < 3; ++k) {
            const uint32_t slot = column * 3 + k;
            const uint32_t b = sp::kModSlotBase + 3 * slot;
            auto &line = lines.add<SynthRow>();
            line.setAlign(Align::Center);
            line.add<Label>(std::to_string(slot + 1), TextStyle::Caption, TextTone::Muted);
            addParamControl(context_, line, access_, paramOf(b), kKnob, 14, false, false);
            line.add<Label>("->", TextStyle::Caption, TextTone::Muted);
            addParamControl(context_, line, access_, paramOf(b + 1), kKnob, 14, false, false);
            const audio::DeviceParam &amount = paramOf(b + 2);
            addParamControl(context_, line, access_, amount, 32.0f, 14, false, false);
            const DeviceAccess access = access_;
            line.add<LiveLabel>([access, amount] { return paramText(access, amount, paramValue(access, amount)); }, TextStyle::Caption,
                                TextTone::Muted);
        }
    }
}

void SynthView::voiceCard(Container &parent)
{
    auto &body = card(parent, "Voice");
    // Mode on its own line, then the rest in one row
    auto &mode = body.add<SynthRow>();
    mode.setAlign(Align::Start);
    addParamControl(context_, mode, access_, paramOf(sp::kVoiceMode), kKnob);
    auto &line = body.add<SynthRow>();
    line.setAlign(Align::Start);
    const uint32_t ids[] = {sp::kVoices, sp::kGlide, sp::kBendRange, sp::kDrift, sp::kVoiceLevel};
    for (uint32_t id : ids)
        addParamControl(context_, line, access_, paramOf(id), kKnob);
}

// --- Presets ---------------------------------------------------------------------------------

std::string SynthView::presetsSignature() const
{
    const audio::DeviceSetting *setting = access_.setting ? access_.setting() : nullptr;
    // The values hashed (a few hundred numbers a frame at most): a change of any value shows
    unsigned long long hash = 1469598103934665603ull;
    if (setting) {
        for (const auto &[id, value] : setting->values) {
            hash = (hash ^ id) * 1099511628211ull;
            unsigned long long bits = 0;
            std::memcpy(&bits, &value, sizeof bits);
            hash = (hash ^ bits) * 1099511628211ull;
        }
    }
    return (setting ? setting->name : std::string()) + "|" + std::to_string(userVersion_) + "|" + std::to_string(hash);
}

void SynthView::refreshPresets()
{
    presets_.clear();
    const audio::DeviceSetting *setting = access_.setting ? access_.setting() : nullptr;
    const std::string current = setting ? setting->name : std::string();
    const plugin::ParamValues values = fullValues(setting ? setting->values : plugin::ParamValues{});
    for (const audio::SynthPreset &p : audio::synthPresets())
        presets_.push_back({p.category + " / " + p.name, p, false});
    for (const audio::SynthPreset &p : user_)
        presets_.push_back({"User / " + p.name, p, false});

    // The preset of the device's name (the first with it), and whether its values still are it
    const audio::SynthPreset *match = nullptr;
    for (const PresetEntry &e : presets_)
        if (!match && e.preset.name == current)
            match = &e.preset;
    PresetEntry own;
    own.special = true;
    own.preset.name = current;
    if (match) {
        if (fullValues(match->values) != values) {
            own.label = current + " (edited)";
            presets_.insert(presets_.begin(), own);
        }
    } else {
        // A name no preset has: its defaults are Init, anything else is the name, edited
        const audio::SynthPreset *init = audio::findSynthPreset("Init");
        const bool defaults = init && fullValues(init->values) == values;
        own.label = defaults ? std::string("Init") : (current.empty() ? std::string("Custom") : current) + " (edited)";
        presets_.insert(presets_.begin(), own);
    }
    std::vector<std::string> labels;
    for (const PresetEntry &e : presets_)
        labels.push_back(e.label);
    presetDrop_->setOptions(labels);
}

int SynthView::selectedPreset() const
{
    // An entry the device's state is shown by (edited, or no preset's name) is first when there is one
    if (!presets_.empty() && presets_.front().special)
        return 0;
    const audio::DeviceSetting *setting = access_.setting ? access_.setting() : nullptr;
    const std::string current = setting ? setting->name : std::string();
    for (size_t i = 0; i < presets_.size(); ++i)
        if (presets_[i].preset.name == current)
            return static_cast<int>(i);
    return 0;
}

void SynthView::choosePreset(int index)
{
    if (index < 0 || static_cast<size_t>(index) >= presets_.size() || presets_[static_cast<size_t>(index)].special)
        return;
    const audio::SynthPreset &preset = presets_[static_cast<size_t>(index)].preset;
    const plugin::ParamValues values = fullValues(preset.values);
    const audio::DeviceSetting *now = access_.setting ? access_.setting() : nullptr;
    if (now && now->name == preset.name && now->values == values)
        return;
    // One edit: the values and the name together
    if (audio::DeviceSetting *s = access_.edit ? access_.edit() : nullptr) {
        s->name = preset.name;
        s->values = values;
    }
}

void SynthView::stepPreset(int delta)
{
    const int n = static_cast<int>(presets_.size());
    if (n == 0)
        return;
    int i = selectedPreset();
    for (int step = 0; step < n; ++step) {
        i = (i + delta + n) % n;
        if (!presets_[static_cast<size_t>(i)].special)
            break;
    }
    choosePreset(i);
}

void SynthView::initPreset()
{
    for (size_t i = 0; i < presets_.size(); ++i)
        if (!presets_[i].special && presets_[i].preset.name == "Init" && presets_[i].preset.category == "Init") {
            choosePreset(static_cast<int>(i));
            return;
        }
}

void SynthView::openSaveDialog()
{
    const audio::DeviceSetting *now = access_.setting ? access_.setting() : nullptr;
    if (!now)
        return;
    auto name = std::make_shared<std::string>(now->name);
    auto values = std::make_shared<plugin::ParamValues>(fullValues(now->values));
    Dialog &dialog = Dialog::open(context_, "Save preset",
                                  "The current values are saved as a preset of your own, in " + userSynthPresetFolder() + ".");
    dialog.setWidth(460.0f);
    TextField &field = dialog.body().add<TextField>("Preset name");
    field.setText(*name).setSize(Size::Small);
    field.onChange([name](const std::string &text) { *name = text; });
    dialog.addButton("Cancel");
    dialog.addButton(
        "Save",
        [this, name, values] {
            audio::SynthPreset preset;
            preset.name = *name;
            preset.category = "User";
            preset.values = *values;
            std::string error;
            if (!saveUserSynthPreset(preset, error)) {
                const std::string message = error;
                context_.post([this, message] { Dialog::open(context_, "Preset not saved", message).addButton("OK", {}, true); });
                return;
            }
            // The presets as they are on disk now, and the device named after the one saved
            std::string skipped;
            user_ = loadUserSynthPresets(skipped);
            ++userVersion_;
            if (audio::DeviceSetting *s = access_.edit ? access_.edit() : nullptr)
                s->name = preset.name;
        },
        true);
}

}
