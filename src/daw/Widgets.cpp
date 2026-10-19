#include "Widgets.h"

#include <algorithm>
#include <utility>

#include "Format.h"

namespace daw {

using namespace ui;

// SmallToggle

SmallToggle::SmallToggle(std::string label, std::function<bool()> get, std::function<void(bool)> set)
    : label_(std::move(label)), get_(std::move(get)), set_(std::move(set))
{
}

Vec2 SmallToggle::measure(const Theme &theme) const
{
    const float side = 26.0f;
    const float text = theme.measureText(label_, TextStyle::Caption).x;
    return {std::max(side, text + 16.0f), side};
}

void SmallToggle::handleInput(const Input &input)
{
    on_ = get_ && get_();
    if (click_.update(rect_, input, enabled_)) {
        on_ = !on_;
        if (set_)
            set_(on_);
    }
}

void SmallToggle::draw(DrawList &list, const Theme &theme) const
{
    // The state, the pointer's hover and press, enabled and the label
    Fingerprint key;
    key.add(on_);
    key.add(click_.hovered);
    key.add(click_.held());
    key.add(enabled_);
    key.add(label_);
    cache_.draw(list, theme, rect_, key.value(), [&] {
    if (on_ || click_.held())
        list.add(theme.surface(rect_, Shape::Soft, Elevation::Inset, 0.55f));
    else
        list.add(theme.surface(rect_, Shape::Soft, Elevation::Raised, click_.hovered ? 0.75f : 0.5f, Finish::Convex));
    const Color color = !enabled_ ? theme.mutedTextColor()
                                  : theme.textColor(on_ ? TextTone::Accent : TextTone::Secondary);
    list.add(theme.text(label_, rect_, theme.fontSize(TextStyle::Caption), TextAlign::Center, color));
    });
}

// ScrubField

ScrubField::ScrubField(std::string label, std::function<std::string()> value, std::function<void(int, bool)> step,
                       std::function<bool()> enabled)
    : label_(std::move(label)), value_(std::move(value)), step_(std::move(step)), enabled_(std::move(enabled))
{
}

ScrubField &ScrubField::setValueWidth(float width)
{
    valueWidth_ = width;
    return *this;
}

Vec2 ScrubField::measure(const Theme &theme) const
{
    const float label = theme.measureText(label_, TextStyle::Caption).x;
    return {label + valueWidth_ + 30.0f, 26.0f};
}

void ScrubField::handleInput(const Input &input)
{
    on_ = !enabled_ || enabled_();
    shown_ = value_ ? value_() : std::string();
    hovered_ = on_ && rect_.contains(input.mouse);
    if (!on_) {
        dragging_ = false;
        return;
    }
    const bool fine = input.shift;
    if (hovered_ && input.pressed) {
        dragging_ = true;
        pressY_ = input.mouse.y;
        stepsDone_ = 0;
    }
    if (dragging_) {
        if (!input.down) {
            dragging_ = false;
        } else {
            // A step every 7 pixels up (down goes the other way)
            const int steps = static_cast<int>(std::floor((pressY_ - input.mouse.y) / 7.0f));
            if (steps != stepsDone_ && step_)
                step_(steps - stepsDone_, fine);
            stepsDone_ = steps;
        }
    }
    if (hovered_ && input.scroll != 0.0f) {
        if (step_)
            step_(input.scroll > 0.0f ? 1 : -1, fine);
        input.scroll = 0.0f;
    }
    if (hovered_ || dragging_)
        input.setCursor(engine::Cursor::ResizeY);
}

void ScrubField::draw(DrawList &list, const Theme &theme) const
{
    list.add(theme.surface(rect_, Shape::Pill, Elevation::Inset, dragging_ ? 0.75f : hovered_ ? 0.6f : 0.45f));
    const Rect inner{rect_.x + 10.0f, rect_.y, rect_.w - 20.0f, rect_.h};
    list.add(theme.text(label_, inner, TextStyle::Caption, TextTone::Muted, TextAlign::Start));
    const TextTone tone = !on_ ? TextTone::Muted : dragging_ ? TextTone::Accent : TextTone::Primary;
    list.add(theme.text(on_ ? shown_ : std::string("-"), inner, TextStyle::Caption, tone, TextAlign::End));
}

// Fader

namespace {

constexpr float kCapHeight = 20.0f;
constexpr float kCapWidth = 32.0f;
constexpr float kSlotWidth = 8.0f;

}

Fader::Fader(std::function<float()> get, std::function<void(float)> set)
    : get_(std::move(get)), set_(std::move(set))
{
}

Fader &Fader::setMaxGain(float gain)
{
    maxGain_ = std::max(0.1f, gain);
    return *this;
}

Fader &Fader::setDefault(float position)
{
    default_ = std::clamp(position, 0.0f, 1.0f);
    return *this;
}

Vec2 Fader::measure(const Theme &) const
{
    return {kCapWidth + 16.0f, 72.0f};
}

float Fader::travelTop() const
{
    return rect_.y + kCapHeight * 0.5f;
}

float Fader::travelLength() const
{
    return std::max(1.0f, rect_.h - kCapHeight);
}

float Fader::capY(float position) const
{
    return travelTop() + (1.0f - position) * travelLength();
}

void Fader::change(float position)
{
    position = std::clamp(position, 0.0f, 1.0f);
    if (position == value_)
        return;
    value_ = position;
    if (set_)
        set_(value_);
}

void Fader::handleInput(const Input &input)
{
    if (!dragging_ && get_)
        value_ = std::clamp(get_(), 0.0f, 1.0f);
    if (!enabled_) {
        hovered_ = dragging_ = false;
        return;
    }
    hovered_ = rect_.contains(input.mouse);
    if (input.pressed && hovered_) {
        if (lastPress_ >= 0.0 && input.time - lastPress_ < 0.3) {
            change(default_);
            lastPress_ = -1.0;
            return;
        }
        lastPress_ = input.time;
        const float cap = capY(value_);
        // Pressing the slot away from the cap jumps the cap there first
        if (std::abs(input.mouse.y - cap) > kCapHeight * 0.5f)
            change(1.0f - (input.mouse.y - travelTop()) / travelLength());
        grab_ = input.mouse.y - capY(value_);
        dragging_ = true;
    }
    if (!input.down)
        dragging_ = false;
    if (dragging_)
        change(1.0f - (input.mouse.y - grab_ - travelTop()) / travelLength());

    if (hovered_ && input.scroll != 0.0f) {
        change(value_ + input.scroll * 0.02f);
        input.scroll = 0.0f;
    }
}

void Fader::draw(DrawList &list, const Theme &theme) const
{
    const float cx = rect_.x + rect_.w * 0.5f;
    const Rect slot{cx - kSlotWidth * 0.5f, rect_.y + kCapHeight * 0.25f, kSlotWidth, rect_.h - kCapHeight * 0.5f};
    list.add(theme.surface(slot, Shape::Pill, Elevation::Inset, 0.5f));

    // dB marks either side of the slot; unity is longer
    const float marks[] = {6.0f, 0.0f, -6.0f, -12.0f, -24.0f, -48.0f};
    for (float db : marks) {
        const float gain = std::pow(10.0f, db / 20.0f);
        if (gain > maxGain_ * 1.001f)
            continue;
        const float y = capY(faderPosition(gain, maxGain_));
        const float length = db == 0.0f ? 7.0f : 4.0f;
        for (float side : {-1.0f, 1.0f}) {
            const float x = side < 0.0f ? slot.x - 4.0f - length : slot.x + slot.w + 4.0f;
            list.add(theme.ink({x, y - 1.0f, length, 2.0f}, Shape::Pill, TextTone::Muted, enabled_));
        }
    }

    const float y = capY(value_);
    const float fillTop = y;
    const float fillBottom = slot.y + slot.h - 2.0f;
    if (fillBottom - fillTop > 2.0f)
        list.add(theme.accent({slot.x + 2.0f, fillTop, slot.w - 4.0f, fillBottom - fillTop}, Shape::Pill,
                              enabled_ ? 0.75f : 0.3f));

    const Rect cap{cx - kCapWidth * 0.5f, y - kCapHeight * 0.5f, kCapWidth, kCapHeight};
    const float depth = !enabled_ ? 0.3f : (dragging_ || hovered_ ? 1.0f : 0.75f);
    list.add(theme.surface(cap, Shape::Soft, Elevation::Raised, depth, Finish::Convex));
    list.add(theme.ink({cx - 9.0f, y - 1.0f, 18.0f, 2.0f}, Shape::Pill, dragging_ ? TextTone::Accent : TextTone::Secondary,
                       enabled_));
}

// Readout

Readout::Readout(std::function<std::string()> main, std::function<std::string()> sub, std::string widest)
    : mainText_(std::move(main)), subText_(std::move(sub)), widest_(std::move(widest))
{
}

Vec2 Readout::measure(const Theme &theme) const
{
    const float width = theme.measureText(widest_, TextStyle::Title).x;
    return {width + 36.0f, theme.fontSize(TextStyle::Title) + theme.fontSize(TextStyle::Caption) + 22.0f};
}

void Readout::handleInput(const Input &)
{
    main_ = mainText_ ? mainText_() : std::string();
    sub_ = subText_ ? subText_() : std::string();
}

void Readout::draw(DrawList &list, const Theme &theme) const
{
    list.add(theme.surface(rect_, Shape::Soft, Elevation::Inset, 0.7f));
    const float titlePx = theme.fontSize(TextStyle::Title);
    const float captionPx = theme.fontSize(TextStyle::Caption);
    const float top = rect_.y + (rect_.h - titlePx - captionPx - 6.0f) * 0.5f;
    const Rect upper{rect_.x, top, rect_.w, titlePx};
    const Rect lower{rect_.x, top + titlePx + 6.0f, rect_.w, captionPx};
    list.add(theme.text(main_, upper, TextStyle::Title, TextTone::Primary, TextAlign::Center));
    list.add(theme.text(sub_, lower, TextStyle::Caption, TextTone::Secondary, TextAlign::Center));
}

// LiveLabel

void ActivityLight::draw(DrawList &list, const Theme &theme) const
{
    const float d = std::min(rect_.w, rect_.h);
    const Rect dot{rect_.x + (rect_.w - d) * 0.5f, rect_.y + (rect_.h - d) * 0.5f, d, d};
    if (on_)
        list.add(theme.accent(dot, Shape::Circle));
    else
        list.add(theme.surface(dot, Shape::Circle, Elevation::Inset, 0.5f));
}

Vec2 LogoMark::measure(const Theme &theme) const
{
    const float d = theme.controlHeight(Size::Small);
    return {d, d};
}

void LogoMark::draw(DrawList &list, const Theme &theme) const
{
    const float d = std::min(rect_.w, rect_.h);
    const Rect disc{rect_.x + (rect_.w - d) * 0.5f, rect_.y + (rect_.h - d) * 0.5f, d, d};
    list.add(theme.surface(disc, Shape::Circle, Elevation::Raised, 0.7f, Finish::Convex));
    drawIcon(list, theme, Icon::Wave, {disc.x + d * 0.2f, disc.y + d * 0.2f, d * 0.6f, d * 0.6f}, TextTone::Accent);
}

ProjectBadge::ProjectBadge(std::function<std::string()> name, std::function<bool()> modified)
    : Label(name ? name() : std::string(), TextStyle::Caption, TextTone::Secondary), name_(std::move(name)),
      modified_(std::move(modified))
{
    setAlignment(TextAlign::Center);
}

void ProjectBadge::handleInput(const Input &)
{
    if (name_)
        setText(name_());
    dirty_ = modified_ && modified_();
}

Vec2 ProjectBadge::measure(const Theme &theme) const
{
    const Vec2 text = theme.measureText(this->text(), TextStyle::Caption);
    return {std::max(160.0f, text.x + 56.0f), theme.controlHeight(Size::Small) - 4.0f};
}

void ProjectBadge::draw(DrawList &list, const Theme &theme) const
{
    list.add(theme.surface(rect_, Shape::Pill, Elevation::Inset, 0.55f));
    const float dot = 7.0f;
    const Vec2 text = theme.measureText(this->text(), TextStyle::Caption);
    const float x = rect_.x + (rect_.w - text.x) * 0.5f - dot - 8.0f;
    list.add(theme.ink({x, rect_.y + (rect_.h - dot) * 0.5f, dot, dot}, Shape::Circle,
                       dirty_ ? TextTone::Accent : TextTone::Muted, true, dirty_ ? 1.0f : 0.5f));
    if (dirty_) {
        Surface accent = theme.accent({x, rect_.y + (rect_.h - dot) * 0.5f, dot, dot}, Shape::Circle);
        list.add(accent);
    }
    Label::draw(list, theme);
}

LiveLabel::LiveLabel(std::function<std::string()> text, TextStyle style, TextTone tone)
    : Label(text ? text() : std::string(), style, tone), source_(std::move(text))
{
}

void LiveLabel::handleInput(const Input &)
{
    if (source_)
        setText(source_());
}

}
