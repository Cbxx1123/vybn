#include "daw/widgets/Widgets.h"

#include <algorithm>
#include <cmath>
#include <utility>

#include "daw/widgets/Format.h"

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

// FoldChevron

FoldChevron::FoldChevron(std::function<bool()> folded, std::function<void(bool)> set)
    : folded_(std::move(folded)), set_(std::move(set))
{
}

Vec2 FoldChevron::measure(const Theme &) const
{
    return {16.0f, 14.0f};
}

void FoldChevron::handleInput(const Input &input)
{
    if (click_.update(rect_, input, enabled_) && set_)
        set_(!(folded_ && folded_()));
}

void FoldChevron::draw(DrawList &list, const Theme &theme) const
{
    const bool folded = folded_ && folded_();
    Fingerprint key;
    key.add(folded);
    key.add(click_.hovered);
    key.add(click_.held());
    cache_.draw(list, theme, rect_, key.value(), [&] {
        if (click_.hovered || click_.held())
            list.add(theme.surface(rect_, Shape::Soft, Elevation::Inset, click_.held() ? 0.55f : 0.3f));
        const Vec2 c{rect_.x + rect_.w * 0.5f, rect_.y + rect_.h * 0.5f};
        const TextTone tone = folded ? TextTone::Accent : TextTone::Secondary;
        if (folded) { // pointing right
            list.add(theme.inkLine({c.x - 2.0f, c.y - 4.5f}, {c.x + 2.5f, c.y}, 1.6f, tone));
            list.add(theme.inkLine({c.x + 2.5f, c.y}, {c.x - 2.0f, c.y + 4.5f}, 1.6f, tone));
        } else { // pointing down
            list.add(theme.inkLine({c.x - 4.5f, c.y - 2.0f}, {c.x, c.y + 2.5f}, 1.6f, tone));
            list.add(theme.inkLine({c.x, c.y + 2.5f}, {c.x + 4.5f, c.y - 2.0f}, 1.6f, tone));
        }
    });
}

// RadialToggles

namespace {

Rect circleAt(Vec2 c, float r)
{
    return {c.x - r, c.y - r, r * 2.0f, r * 2.0f};
}

float distance(Vec2 a, Vec2 b)
{
    return std::hypot(a.x - b.x, a.y - b.y);
}

}

RadialToggles::RadialToggles(std::vector<RadialToggle> toggles) : toggles_(std::move(toggles)), on_(toggles_.size(), 0)
{
}

float RadialToggles::angle(size_t i, size_t count)
{
    const float step = 6.2831853f / static_cast<float>(std::max<size_t>(count, 1));
    return -1.5707963f + step * static_cast<float>(i);
}

Vec2 RadialToggles::measure(const Theme &) const
{
    return {26.0f, 26.0f};
}

bool RadialToggles::hitTest(Vec2 p) const
{
    return distance(p, rect_.center()) <= rect_.w * 0.5f;
}

void RadialToggles::handleInput(const Input &input)
{
    for (size_t i = 0; i < toggles_.size(); ++i)
        on_[i] = toggles_[i].get && toggles_[i].get();
    hovered_ = enabled_ && hitTest(input.mouse);
    if (hovered_)
        input.setCursor(engine::Cursor::Hand);
    if (hovered_ && input.pressed && !*open_)
        if (Context *context = Context::active())
            openMenu(*context, true);
}

RadialMenu &RadialToggles::openMenu(Context &context, bool held)
{
    *open_ = true;
    OverlayOptions options;
    options.onClose = [open = open_] { *open = false; };
    return context.openOverlay<RadialMenu>(std::move(options), context, toggles_, rect_.center(), held);
}

void RadialToggles::draw(DrawList &list, const Theme &theme) const
{
    Fingerprint key;
    for (char on : on_)
        key.add(on);
    key.add(hovered_);
    key.add(*open_);
    key.add(enabled_);
    cache_.draw(list, theme, rect_, key.value(), [&] {
        const Vec2 c = rect_.center();
        const float r = rect_.w * 0.5f;
        if (*open_)
            list.add(theme.surface(rect_, Shape::Circle, Elevation::Inset, 0.55f));
        else
            list.add(theme.surface(rect_, Shape::Circle, Elevation::Raised, hovered_ ? 0.75f : 0.5f, Finish::Convex));
        // A dot per toggle where its button sits in the menu, lit while it is on
        for (size_t i = 0; i < toggles_.size(); ++i) {
            const float a = angle(i, toggles_.size());
            const Rect dot = circleAt({c.x + std::cos(a) * r * 0.5f, c.y + std::sin(a) * r * 0.5f}, 2.5f);
            if (on_[i] && enabled_)
                list.add(theme.accent(dot, Shape::Circle));
            else
                list.add(theme.ink(dot, Shape::Circle, TextTone::Muted, enabled_, 0.55f));
        }
    });
}

// RadialMenu

RadialMenu::RadialMenu(Context &context, std::vector<RadialToggle> toggles, Vec2 centre, bool held)
    : context_(context), toggles_(std::move(toggles)), anchor_(centre), centre_(centre), held_(held)
{
}

Vec2 RadialMenu::measure(const Theme &) const
{
    return {radius() * 2.0f, radius() * 2.0f};
}

void RadialMenu::arrange(const Rect &window, const Theme &theme)
{
    // Kept inside the window, room left beneath for the name
    const float r = radius() + 4.0f;
    const float below = theme.fontSize(TextStyle::Caption) + 24.0f;
    centre_.x = std::clamp(anchor_.x, window.x + r, std::max(window.x + r, window.x + window.w - r));
    centre_.y = std::clamp(anchor_.y, window.y + r, std::max(window.y + r, window.y + window.h - r - below));
    rect_ = circleAt(centre_, radius());
}

Rect RadialMenu::button(size_t i) const
{
    const float a = RadialToggles::angle(i, toggles_.size());
    const float ring = kRing * reveal_;
    return circleAt({centre_.x + std::cos(a) * ring, centre_.y + std::sin(a) * ring}, kButton * 0.5f);
}

bool RadialMenu::hitTest(Vec2 p) const
{
    return distance(p, centre_) <= radius();
}

void RadialMenu::flip(size_t i)
{
    const RadialToggle &t = toggles_[i];
    const bool on = t.get && t.get();
    if (t.set)
        t.set(!on);
}

void RadialMenu::close()
{
    context_.closeOverlay(*this);
}

void RadialMenu::handleInput(const Input &input)
{
    if (reveal_ < 1.0f) {
        reveal_ += (1.0f - reveal_) * input.ease(18.0f);
        if (reveal_ > 0.995f)
            reveal_ = 1.0f;
    }
    // Escape closes it and a toggle's key flips it; other keys go on (Space still plays)
    if (hooks_.attach(input.signals)) {
        hooks_.key(Priority::Popup, [this](const KeyEvent &e) {
            if (!e.down || e.mods != 0)
                return false;
            if (e.key == Key::Escape) {
                close();
                return true;
            }
            for (size_t i = 0; i < toggles_.size(); ++i) {
                if (toggles_[i].key == e.key) {
                    if (!e.repeat)
                        flip(i);
                    return true;
                }
            }
            return false;
        });
    }

    hovered_ = -1;
    for (size_t i = 0; i < toggles_.size(); ++i) {
        if (distance(input.mouse, button(i).center()) <= kButton * 0.5f + 3.0f)
            hovered_ = static_cast<int>(i);
    }
    middleHovered_ = distance(input.mouse, centre_) <= kMiddle * 0.5f;
    if (hovered_ >= 0 || middleHovered_)
        input.setCursor(engine::Cursor::Hand);

    if (held_) {
        // Opened by a press still held: dragging onto a button and letting go flips it
        dragged_ = dragged_ || distance(input.mouse, centre_) > kMiddle * 0.5f;
        if (input.released || !input.down) {
            held_ = false;
            if (hovered_ >= 0 && dragged_) {
                flip(static_cast<size_t>(hovered_));
                close();
            } else if (dragged_ && !middleHovered_) {
                close(); // let go over nothing
            }
        }
        return;
    }
    if (!input.pressed)
        return;
    if (hovered_ >= 0)
        flip(static_cast<size_t>(hovered_));
    else if (middleHovered_)
        close();
}

void RadialMenu::draw(DrawList &list, const Theme &theme) const
{
    const float shown = reveal_;
    // The disc the buttons sit on, growing out of the middle
    const float discR = kMiddle * 0.5f + (radius() - kMiddle * 0.5f) * shown;
    list.add(theme.surface(circleAt(centre_, discR), Shape::Circle, Elevation::Raised, 0.8f, Finish::Flat));
    // The middle, where the disc that opened it is: pressed in, with a cross that closes
    const Rect middle = circleAt(centre_, kMiddle * 0.5f);
    list.add(theme.surface(middle, Shape::Circle, Elevation::Inset, middleHovered_ ? 0.7f : 0.5f));
    drawIcon(list, theme, Icon::Close, circleAt(centre_, 5.0f), middleHovered_ ? TextTone::Primary : TextTone::Muted);

    const float textSize = theme.fontSize(TextStyle::Caption);
    for (size_t i = 0; i < toggles_.size(); ++i) {
        const RadialToggle &t = toggles_[i];
        const bool on = t.get && t.get();
        const bool hovered = static_cast<int>(i) == hovered_;
        const Rect r = button(i);
        if (on)
            list.add(theme.surface(r, Shape::Circle, Elevation::Inset, hovered ? 0.7f : 0.55f));
        else
            list.add(theme.surface(r, Shape::Circle, Elevation::Raised, hovered ? 0.8f : 0.5f, Finish::Convex));
        Color color = theme.textColor(on ? TextTone::Accent : hovered ? TextTone::Primary : TextTone::Secondary);
        color.a *= shown;
        list.add(theme.text(t.label, r, textSize, TextAlign::Center, color));
    }

    // The hovered toggle's name (and whether it is on), in a pill under the disc
    if (hovered_ >= 0 && shown > 0.9f) {
        const RadialToggle &t = toggles_[static_cast<size_t>(hovered_)];
        const std::string text = t.name + ((t.get && t.get()) ? "  On" : "  Off");
        const float w = theme.measureText(text, TextStyle::Caption).x + 20.0f;
        const float h = textSize + 12.0f;
        const Rect pill{centre_.x - w * 0.5f, centre_.y + radius() + 6.0f, w, h};
        list.add(theme.surface(pill, Shape::Pill, Elevation::Raised, 0.7f));
        list.add(theme.text(text, pill, textSize, TextAlign::Center, theme.textColor(TextTone::Primary)));
    }
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
