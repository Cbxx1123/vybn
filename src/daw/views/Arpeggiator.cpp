#include "daw/views/Arpeggiator.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <string>
#include <utility>

#include "daw/widgets/Format.h"
#include "daw/widgets/Widgets.h"

namespace daw {

using namespace ui;

namespace {

constexpr size_t kMaxSteps = 16;
constexpr float kCell = 26.0f;
constexpr float kCellGap = 4.0f;

// The rhythm with at least one step, every step one of the four marks
std::string steps(const std::string &rhythm)
{
    std::string out;
    for (char ch : rhythm) {
        if (ch == 'x' || ch == 'X' || ch == '-' || ch == '.')
            out += ch;
    }
    return out.empty() ? std::string("x") : out.substr(0, kMaxSteps);
}

}

// --- The steps ------------------------------------------------------------------------------

Vec2 ArpSteps::measure(const Theme &) const
{
    return {kMaxSteps * (kCell + kCellGap), kCell + 4.0f};
}

Rect ArpSteps::cell(size_t index) const
{
    return {rect_.x + static_cast<float>(index) * (kCell + kCellGap), rect_.y + 2.0f, kCell, kCell};
}

void ArpSteps::handleInput(const Input &input)
{
    const std::string marks = steps(rhythm_);
    hovered_ = -1;
    for (size_t i = 0; i < marks.size(); ++i) {
        if (cell(i).contains(input.mouse))
            hovered_ = static_cast<int>(i);
    }
    if (hovered_ >= 0)
        input.setCursor(engine::Cursor::Hand);
    if (hovered_ >= 0 && input.pressed) {
        // Play, accent, tie, rest, and round again (the first step can't be tied to nothing)
        std::string next = marks;
        char &mark = next[static_cast<size_t>(hovered_)];
        switch (mark) {
        case 'x': mark = 'X'; break;
        case 'X': mark = hovered_ == 0 ? '.' : '-'; break;
        case '-': mark = '.'; break;
        default: mark = 'x'; break;
        }
        rhythm_ = next;
    }
}

void ArpSteps::draw(DrawList &list, const Theme &theme) const
{
    const std::string marks = steps(rhythm_);
    for (size_t i = 0; i < marks.size(); ++i) {
        const Rect r = cell(i);
        const bool hot = static_cast<int>(i) == hovered_;
        switch (marks[i]) {
        case 'x':
            list.add(theme.surface(r, Shape::Soft, Elevation::Raised, hot ? 0.8f : 0.55f, Finish::Convex));
            list.add(theme.accent(r.inset(8.0f), Shape::Circle, 0.55f));
            break;
        case 'X':
            list.add(theme.surface(r, Shape::Soft, Elevation::Raised, hot ? 0.8f : 0.55f, Finish::Convex));
            list.add(theme.accent(r.inset(3.0f), Shape::Soft, 0.95f));
            break;
        case '-':
            // A tie: a bar reaching back into the step before
            list.add(theme.surface(r, Shape::Soft, Elevation::Inset, 0.4f));
            list.add(theme.accent({r.x - kCellGap - 4.0f, r.y + r.h * 0.5f - 2.0f, r.w * 0.7f + kCellGap, 4.0f}, Shape::Pill, 0.8f));
            break;
        default:
            list.add(theme.surface(r, Shape::Soft, Elevation::Inset, hot ? 0.7f : 0.5f));
            break;
        }
        // Every fourth step marked under it, as beats are
        if (i % 4 == 0)
            list.add(theme.ink({r.x, r.y + r.h + 1.0f, r.w, 1.5f}, Shape::Pill, TextTone::Muted, true, 0.5f));
    }
}

// --- The preview ----------------------------------------------------------------------------

void ArpPreview::handleInput(const Input &)
{
    before_ = notes_ ? notes_() : std::vector<Note>{};
    after_ = before_;
    arpeggiate(after_, settings_);
    // The made notes are the selected ones; show from the first of them, two bars at most
    from_ = 1e30;
    double last = -1e30;
    low_ = 127;
    high_ = 0;
    for (const Note &n : after_) {
        if (!n.selected)
            continue;
        from_ = std::min(from_, n.start);
        last = std::max(last, n.start + n.length);
    }
    if (last < from_) {
        from_ = 0.0;
        last = 4.0;
    }
    to_ = std::min(last, from_ + 8.0);
    for (const std::vector<Note> *set : {&before_, &after_}) {
        for (const Note &n : *set) {
            if (n.start < to_ && n.start + n.length > from_ && (set == &after_ ? n.selected : true)) {
                low_ = std::min(low_, n.key);
                high_ = std::max(high_, n.key);
            }
        }
    }
    if (high_ < low_) {
        low_ = 60;
        high_ = 72;
    }
    if (high_ - low_ < 12) {
        low_ = std::max(0, low_ - (12 - (high_ - low_)) / 2);
        high_ = std::min(127, low_ + 12);
    }
}

void ArpPreview::draw(DrawList &list, const Theme &theme) const
{
    // The well, with a line under it saying what it shows
    const Rect well{rect_.x, rect_.y, rect_.w, rect_.h - 20.0f};
    list.add(theme.surface(well, Shape::Soft, Elevation::Inset, 0.6f));
    const Rect area = well.inset(8.0f);
    const double span = std::max(0.25, to_ - from_);
    const float rows = static_cast<float>(high_ - low_ + 1);
    const float rowHeight = area.h / rows;
    auto x = [&](double beat) { return area.x + static_cast<float>((beat - from_) / span) * area.w; };
    auto y = [&](int key) { return area.y + static_cast<float>(high_ - key) * rowHeight; };
    list.pushClip(well.inset(2.0f));
    // Steps and beats
    for (double b = from_; b <= to_ + 1e-9; b += settings_.rate) {
        const bool beat = std::fabs(b - std::round(b)) < 1e-6;
        list.add(theme.ink({x(b), area.y, 1.0f, area.h}, Shape::Rounded, TextTone::Muted, true, beat ? 0.25f : 0.08f));
    }
    // The chords it starts from, faint; what it makes, bright by velocity, as raised pills
    for (const Note &n : before_) {
        if (n.start >= to_ || n.start + n.length <= from_)
            continue;
        const Rect r{x(n.start), y(n.key) + 0.5f, std::max(3.0f, x(n.start + n.length) - x(n.start)), std::max(2.0f, rowHeight - 1.0f)};
        list.add(theme.ink(r, Shape::Soft, TextTone::Secondary, true, 0.14f));
    }
    for (const Note &n : after_) {
        if (!n.selected || n.start >= to_ || n.start + n.length <= from_)
            continue;
        const Rect r{x(n.start), y(n.key) + 0.5f, std::max(3.0f, x(n.start + n.length) - x(n.start) - 1.0f), std::max(2.0f, rowHeight - 1.0f)};
        if (r.h >= 5.0f)
            list.add(theme.surface(r, Shape::Soft, Elevation::Raised, 0.3f, Finish::Convex));
        list.add(theme.accent(r, Shape::Soft, 0.3f + 0.7f * n.velocity));
    }
    list.popClip();
    int made = 0;
    for (const Note &n : after_)
        made += n.selected ? 1 : 0;
    const std::string caption = made == 0 ? std::string("Select a chord (or have some notes) to arpeggiate")
                                          : format("%.0f notes", made) + (to_ - from_ < 8.0 - 1e-6 ? "" : ", the first two bars shown");
    list.add(theme.text(caption, {rect_.x + 4.0f, rect_.y + rect_.h - 16.0f, rect_.w - 8.0f, 16.0f}, TextStyle::Caption,
                        TextTone::Muted, TextAlign::End));
}

// --- The dialog -----------------------------------------------------------------------------

void openArpeggiator(Context &context, ArpSettings &settings, std::function<std::vector<Note>()> notes,
                     std::function<void()> apply)
{
    Dialog &dialog = Dialog::open(context, "Arpeggiate",
                                  "Notes that sound together become a pattern through their keys. It works on the "
                                  "selected notes, or all of them.");
    dialog.setWidth(660.0f);
    Container &body = dialog.body();
    auto labelled = [&body](const char *label) -> Container & {
        auto &r = body.add<Container>(Direction::Row);
        r.setAlign(Align::Center);
        r.add<Label>(label, TextStyle::Caption, TextTone::Secondary).setMinSize({76.0f, 0.0f});
        return r;
    };
    ArpSettings *s = &settings;

    // Presets, then each control following the settings every frame (so a preset shows at once)
    std::vector<std::string> presetNames{"Choose a preset..."};
    for (const ArpPreset &p : arpPresets())
        presetNames.push_back(p.name);
    auto &preset = labelled("Preset").add<Dropdown>(context, presetNames, 0, [s](int i) {
        if (i <= 0)
            return;
        const uint32_t seed = s->seed;
        *s = arpPresets()[static_cast<size_t>(i - 1)].settings;
        s->seed = seed;
    });
    preset.setSize(Size::Small).setGrow(1.0f);

    auto &patternRow = labelled("Pattern");
    std::vector<std::string> patternNames;
    for (ArpPattern p : arpPatterns())
        patternNames.push_back(arpPatternName(p));
    auto &pattern = patternRow.add<Dropdown>(context, patternNames, 0, [s](int i) {
        s->pattern = arpPatterns()[static_cast<size_t>(std::clamp(i, 0, static_cast<int>(arpPatterns().size()) - 1))];
    });
    pattern.setSize(Size::Small).setMinSize({200.0f, 0.0f});
    patternRow.add<Spacer>();
    patternRow.add<Label>("Octaves", TextStyle::Caption, TextTone::Secondary);
    auto &octaves = patternRow.add<SegmentedControl>(std::vector<std::string>{"1", "2", "3", "4"}, 0,
                                                     [s](int i) { s->octaves = i + 1; });
    octaves.setSize(Size::Small);
    auto &octaveMode = patternRow.add<SegmentedControl>(std::vector<std::string>{"Up", "Down", "Both"}, 0,
                                                        [s](int i) { s->octaveMode = static_cast<ArpOctaves>(i); });
    octaveMode.setSize(Size::Small);

    auto &rateRow = labelled("Rate");
    const std::vector<double> rates{1.0, 0.5, 1.0 / 3.0, 0.25, 1.0 / 6.0, 0.125};
    auto &rate = rateRow.add<SegmentedControl>(std::vector<std::string>{"1/4", "1/8", "1/8T", "1/16", "1/16T", "1/32"}, 3,
                                               [s, rates](int i) { s->rate = rates[static_cast<size_t>(i)]; });
    rate.setSize(Size::Small);
    rateRow.add<Spacer>();
    rateRow.add<Label>("Ratchet", TextStyle::Caption, TextTone::Secondary);
    auto &repeats = rateRow.add<SegmentedControl>(std::vector<std::string>{"1", "2", "3", "4"}, 0, [s](int i) { s->repeats = i + 1; });
    repeats.setSize(Size::Small);

    // A slider with its value beside it
    auto slider = [&labelled](const char *label, std::function<float()> get, std::function<void(float)> set,
                              std::function<std::string()> text) {
        auto &r = labelled(label);
        auto &sl = r.add<Slider>(get(), set);
        sl.setSize(Size::Small).setGrow(1.0f);
        Slider *raw = &sl;
        r.add<LiveLabel>(text, TextStyle::Caption, TextTone::Secondary).setMinSize({64.0f, 0.0f});
        r.add<FrameHook>([raw, get](const Input &) { raw->setValue(get()); });
    };
    slider("Gate", [s] { return static_cast<float>((s->gate - 0.05) / 1.95); },
           [s](float v) { s->gate = 0.05 + v * 1.95; }, [s] { return format("%.0f%%", s->gate * 100.0); });
    slider("Swing", [s] { return static_cast<float>(s->swing); }, [s](float v) { s->swing = v; },
           [s] { return s->swing <= 0.0 ? std::string("Straight") : format("%.0f%%", s->swing * 100.0); });
    slider("Humanize", [s] { return static_cast<float>(s->humanize); }, [s](float v) { s->humanize = v; },
           [s] { return s->humanize <= 0.0 ? std::string("Exact") : format("%.0f%%", s->humanize * 100.0); });

    auto &velocityRow = labelled("Velocity");
    auto &velocityMode = velocityRow.add<SegmentedControl>(std::vector<std::string>{"As played", "Fixed", "Rising", "Falling"}, 0,
                                                           [s](int i) { s->velocityMode = static_cast<ArpVelocity>(i); });
    velocityMode.setSize(Size::Small);
    velocityRow.add<ScrubField>("Level", [s] { return format("%.0f", std::round(s->velocity * 127.0f)); }, [s](int steps, bool fine) {
        s->velocity = std::clamp(s->velocity + static_cast<float>(steps) * (fine ? 1.0f : 4.0f) / 127.0f, 1.0f / 127.0f, 1.0f);
    }, [s] { return s->velocityMode != ArpVelocity::AsPlayed; }).setValueWidth(26.0f);
    velocityRow.add<ScrubField>("Accent", [s] { return format("+%.0f", std::round(s->accent * 127.0f)); }, [s](int steps, bool fine) {
        s->accent = std::clamp(s->accent + static_cast<float>(steps) * (fine ? 1.0f : 4.0f) / 127.0f, 0.0f, 1.0f);
    }).setValueWidth(30.0f);

    auto &moreRow = labelled("Each round");
    moreRow.add<ScrubField>("Transpose", [s] { return s->transpose == 0 ? std::string("None") : format("%+.0f st", s->transpose); },
                            [s](int steps, bool fine) { s->transpose = std::clamp(s->transpose + steps * (fine ? 12 : 1), -12, 12); })
        .setValueWidth(46.0f);
    moreRow.add<ScrubField>("Lasts", [s] { return s->length <= 0.0 ? std::string("The chord") : format("%.3g beats", s->length); },
                            [s](int steps, bool fine) { s->length = std::clamp(s->length + steps * (fine ? 0.25 : 1.0), 0.0, 64.0); })
        .setValueWidth(70.0f);
    moreRow.add<Spacer>();
    // Random choices are seeded, so the preview is what Arpeggiate makes; this rolls again
    moreRow.add<Button>([s] { s->seed = s->seed * 1664525u + 1013904223u; })
        .setLabel("New random")
        .setVariant(ButtonVariant::Flat)
        .setSize(Size::Small);

    // The rhythm: its steps, how many, and a reset
    auto &rhythmRow = labelled("Rhythm");
    rhythmRow.add<ArpSteps>(settings.rhythm);
    rhythmRow.add<Spacer>();
    rhythmRow.add<ScrubField>("Steps", [s] { return format("%.0f", static_cast<double>(steps(s->rhythm).size())); },
                              [s](int change, bool) {
                                  std::string marks = steps(s->rhythm);
                                  const int count = std::clamp(static_cast<int>(marks.size()) + change, 1, static_cast<int>(kMaxSteps));
                                  marks.resize(static_cast<size_t>(count), 'x');
                                  s->rhythm = marks;
                              })
        .setValueWidth(20.0f);
    rhythmRow.add<Button>([s] { s->rhythm = "xxxxxxxx"; }).setLabel("Every step").setVariant(ButtonVariant::Flat).setSize(Size::Small);
    body.add<Label>("Click a step: play, accent, tie to the one before, rest", TextStyle::Caption, TextTone::Muted);

    body.add<ArpPreview>(settings, notes);

    // Follow the settings (presets change them all)
    Dropdown *patternRaw = &pattern;
    SegmentedControl *octavesRaw = &octaves, *octaveModeRaw = &octaveMode, *rateRaw = &rate, *repeatsRaw = &repeats,
                     *velocityRaw = &velocityMode;
    body.add<FrameHook>([s, rates, patternRaw, octavesRaw, octaveModeRaw, rateRaw, repeatsRaw, velocityRaw](const Input &) {
        const auto &all = arpPatterns();
        patternRaw->setSelected(static_cast<int>(std::find(all.begin(), all.end(), s->pattern) - all.begin()));
        octavesRaw->setSelected(std::clamp(s->octaves, 1, 4) - 1);
        octaveModeRaw->setSelected(static_cast<int>(s->octaveMode));
        for (size_t i = 0; i < rates.size(); ++i) {
            if (std::fabs(rates[i] - s->rate) < 1e-6)
                rateRaw->setSelected(static_cast<int>(i));
        }
        repeatsRaw->setSelected(std::clamp(s->repeats, 1, 4) - 1);
        velocityRaw->setSelected(static_cast<int>(s->velocityMode));
    });

    dialog.addButton("Cancel");
    dialog.addButton("Arpeggiate", [apply] { apply(); }, true);
}

}
