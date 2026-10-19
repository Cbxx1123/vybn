#include "Controls.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <utility>

#include "Components.h"

namespace ui {

namespace {

constexpr float kPi = 3.14159265f;

Rect square(Vec2 centre, float side)
{
    return {centre.x - side * 0.5f, centre.y - side * 0.5f, side, side};
}

// Index of the rect containing `p`, or -1
int indexAt(const std::vector<Rect> &rects, Vec2 p)
{
    for (size_t i = 0; i < rects.size(); ++i) {
        if (rects[i].contains(p))
            return static_cast<int>(i);
    }
    return -1;
}

}

// Checkbox

Checkbox::Checkbox(std::string label, bool checked, std::function<void(bool)> onChange)
    : label_(std::move(label)), onChange_(std::move(onChange)), checked_(checked), fill_(checked ? 1.0f : 0.0f)
{
}

Checkbox &Checkbox::onChange(std::function<void(bool)> callback)
{
    onChange_ = std::move(callback);
    return *this;
}

Checkbox &Checkbox::setChecked(bool checked)
{
    checked_ = checked;
    return *this;
}

Checkbox &Checkbox::setSize(Size size)
{
    size_ = size;
    return *this;
}

float Checkbox::boxSize(const Theme &theme) const
{
    return theme.controlHeight(size_) * 0.58f;
}

Vec2 Checkbox::measure(const Theme &theme) const
{
    const float box = boxSize(theme);
    if (label_.empty())
        return {box, box};
    const Vec2 text = theme.measureText(label_, size_);
    return {box + theme.fontSize(size_) * 0.6f + text.x, std::max(box, text.y)};
}

void Checkbox::handleInput(const Input &input)
{
    fill_ += ((checked_ ? 1.0f : 0.0f) - fill_) * input.ease();
    if (click_.update(rect_, input, enabled_)) {
        checked_ = !checked_;
        if (onChange_)
            onChange_(checked_);
    }
}

void Checkbox::draw(DrawList &list, const Theme &theme) const
{
    const float box = rect_.h > 0.0f ? std::min(rect_.h, boxSize(theme)) : boxSize(theme);
    const Rect r{rect_.x, rect_.y + (rect_.h - box) * 0.5f, box, box};

    // Unchecked boxes stand up; checked ones are pressed in and filled
    if (checked_)
        list.add(theme.surface(r, Shape::Soft, Elevation::Inset, 0.5f));
    else
        list.add(theme.surface(r, Shape::Soft, Elevation::Raised, click_.hovered ? 0.55f : 0.4f, Finish::Convex));
    if (fill_ > 0.02f) {
        const float side = box * 0.5f * fill_;
        list.add(theme.accent(square(r.center(), side), Shape::Soft, enabled_ ? 0.9f : 0.4f));
    }

    if (!label_.empty()) {
        const float x = r.x + box + theme.fontSize(size_) * 0.6f;
        list.add(theme.text(label_, {x, rect_.y, rect_.x + rect_.w - x, rect_.h}, size_, TextAlign::Start, enabled_));
    }
}

// RadioGroup

RadioGroup::RadioGroup(std::vector<std::string> options, int selected, std::function<void(int)> onChange)
    : options_(std::move(options)), onChange_(std::move(onChange)), selected_(selected)
{
}

RadioGroup &RadioGroup::onChange(std::function<void(int)> callback)
{
    onChange_ = std::move(callback);
    return *this;
}

RadioGroup &RadioGroup::setSelected(int index)
{
    selected_ = index;
    return *this;
}

RadioGroup &RadioGroup::setDirection(Direction direction)
{
    direction_ = direction;
    return *this;
}

RadioGroup &RadioGroup::setSize(Size size)
{
    size_ = size;
    return *this;
}

Vec2 RadioGroup::optionSize(const Theme &theme, size_t index) const
{
    const float circle = theme.controlHeight(size_) * 0.55f;
    const Vec2 text = theme.measureText(options_[index], size_);
    return {circle + theme.fontSize(size_) * 0.6f + text.x, std::max(circle, text.y) + 4.0f};
}

float RadioGroup::gap(const Theme &theme) const
{
    return theme.fontSize(size_) * (direction_ == Direction::Row ? 1.6f : 0.7f);
}

Vec2 RadioGroup::measure(const Theme &theme) const
{
    Vec2 total;
    for (size_t i = 0; i < options_.size(); ++i) {
        const Vec2 size = optionSize(theme, i);
        if (direction_ == Direction::Row) {
            total.x += size.x;
            total.y = std::max(total.y, size.y);
        } else {
            total.y += size.y;
            total.x = std::max(total.x, size.x);
        }
    }
    if (options_.size() > 1)
        (direction_ == Direction::Row ? total.x : total.y) += gap(theme) * (options_.size() - 1);
    return total;
}

void RadioGroup::arrange(const Rect &rect, const Theme &theme)
{
    rect_ = rect;
    optionRects_.clear();
    float cursor = direction_ == Direction::Row ? rect.x : rect.y;
    for (size_t i = 0; i < options_.size(); ++i) {
        const Vec2 size = optionSize(theme, i);
        if (direction_ == Direction::Row) {
            optionRects_.push_back({cursor, rect.y + (rect.h - size.y) * 0.5f, size.x, size.y});
            cursor += size.x + gap(theme);
        } else {
            optionRects_.push_back({rect.x, cursor, size.x, size.y});
            cursor += size.y + gap(theme);
        }
    }
}

void RadioGroup::handleInput(const Input &input)
{
    if (!enabled_) {
        hovered_ = pressed_ = -1;
        return;
    }
    hovered_ = indexAt(optionRects_, input.mouse);
    if (input.pressed)
        pressed_ = hovered_;
    if (input.released) {
        if (pressed_ >= 0 && pressed_ == hovered_ && pressed_ != selected_) {
            selected_ = pressed_;
            if (onChange_)
                onChange_(selected_);
        }
        pressed_ = -1;
    }
}

void RadioGroup::draw(DrawList &list, const Theme &theme) const
{
    const float circle = theme.controlHeight(size_) * 0.55f;
    for (size_t i = 0; i < optionRects_.size(); ++i) {
        const Rect &o = optionRects_[i];
        const bool selected = static_cast<int>(i) == selected_;
        const Rect c{o.x, o.y + (o.h - circle) * 0.5f, circle, circle};
        if (selected) {
            list.add(theme.surface(c, Shape::Circle, Elevation::Inset, 0.5f));
            list.add(theme.accent(square(c.center(), circle * 0.42f), Shape::Circle, enabled_ ? 1.0f : 0.4f));
        } else {
            list.add(theme.surface(c, Shape::Circle, Elevation::Raised, hovered_ == static_cast<int>(i) ? 0.55f : 0.4f,
                                   Finish::Convex));
        }
        const float x = c.x + circle + theme.fontSize(size_) * 0.6f;
        list.add(theme.text(options_[i], {x, o.y, o.x + o.w - x, o.h}, size_, TextAlign::Start, enabled_));
    }
}

// SegmentedControl

SegmentedControl::SegmentedControl(std::vector<std::string> options, int selected, std::function<void(int)> onChange)
    : options_(std::move(options)), onChange_(std::move(onChange)), selected_(selected),
      position_(static_cast<float>(selected))
{
}

SegmentedControl &SegmentedControl::onChange(std::function<void(int)> callback)
{
    onChange_ = std::move(callback);
    return *this;
}

SegmentedControl &SegmentedControl::setSelected(int index)
{
    selected_ = index;
    return *this;
}

SegmentedControl &SegmentedControl::setSize(Size size)
{
    size_ = size;
    return *this;
}

Vec2 SegmentedControl::measure(const Theme &theme) const
{
    const float height = theme.controlHeight(size_);
    float widest = 0.0f;
    for (const std::string &option : options_)
        widest = std::max(widest, theme.measureText(option, size_).x);
    const float segmentWidth = widest + height * 0.8f;
    return {segmentWidth * options_.size() + height * 0.2f, height};
}

Rect SegmentedControl::segment(float position) const
{
    const float margin = rect_.h * 0.1f;
    const float width = (rect_.w - 2.0f * margin) / std::max<size_t>(1, options_.size());
    return {rect_.x + margin + position * width, rect_.y + margin, width, rect_.h - 2.0f * margin};
}

void SegmentedControl::handleInput(const Input &input)
{
    position_ += (static_cast<float>(selected_) - position_) * input.ease(14.0f);
    if (!enabled_) {
        hovered_ = pressed_ = -1;
        return;
    }
    hovered_ = -1;
    for (size_t i = 0; i < options_.size(); ++i) {
        if (segment(static_cast<float>(i)).contains(input.mouse))
            hovered_ = static_cast<int>(i);
    }
    if (input.pressed)
        pressed_ = hovered_;
    if (input.released) {
        if (pressed_ >= 0 && pressed_ == hovered_ && pressed_ != selected_) {
            selected_ = pressed_;
            if (onChange_)
                onChange_(selected_);
        }
        pressed_ = -1;
    }
}

void SegmentedControl::draw(DrawList &list, const Theme &theme) const
{
    list.add(theme.surface(rect_, Shape::Pill, Elevation::Inset, 0.6f));
    if (!options_.empty())
        list.add(theme.surface(segment(position_), Shape::Pill, Elevation::Raised, enabled_ ? 0.5f : 0.25f, Finish::Convex));

    for (size_t i = 0; i < options_.size(); ++i) {
        const bool selected = static_cast<int>(i) == selected_;
        const TextTone tone = !enabled_ ? TextTone::Muted
                              : selected ? TextTone::Accent
                              : hovered_ == static_cast<int>(i) ? TextTone::Primary
                                                                : TextTone::Secondary;
        list.add(theme.text(options_[i], segment(static_cast<float>(i)), theme.fontSize(size_), TextAlign::Center,
                            theme.textColor(tone)));
    }
}

// Stepper

Stepper::Stepper(float value, float min, float max, float step, std::function<void(float)> onChange)
    : value_(std::clamp(value, min, max)), min_(min), max_(max), step_(step), onChange_(std::move(onChange))
{
}

Stepper &Stepper::onChange(std::function<void(float)> callback)
{
    onChange_ = std::move(callback);
    return *this;
}

Stepper &Stepper::setValue(float value)
{
    value_ = std::clamp(value, min_, max_);
    return *this;
}

Stepper &Stepper::setSuffix(std::string suffix)
{
    suffix_ = std::move(suffix);
    return *this;
}

Stepper &Stepper::setSize(Size size)
{
    size_ = size;
    return *this;
}

std::string Stepper::format(float value) const
{
    char buffer[32];
    // Whole steps show whole numbers
    if (std::fabs(step_ - std::round(step_)) < 1e-4f)
        std::snprintf(buffer, sizeof(buffer), "%d", static_cast<int>(std::round(value)));
    else
        std::snprintf(buffer, sizeof(buffer), "%.1f", value);
    return buffer + suffix_;
}

void Stepper::change(float delta)
{
    const float value = std::clamp(value_ + delta, min_, max_);
    if (value != value_) {
        value_ = value;
        if (onChange_)
            onChange_(value_);
    }
}

Vec2 Stepper::measure(const Theme &theme) const
{
    const float height = theme.controlHeight(size_);
    // Wide enough for the longest value, so the control doesn't change size while stepping
    const float text = std::max(theme.measureText(format(min_), size_).x, theme.measureText(format(max_), size_).x);
    return {2.0f * height + text + theme.fontSize(size_), height};
}

void Stepper::arrange(const Rect &rect, const Theme &)
{
    rect_ = rect;
    const float margin = rect.h * 0.12f;
    const float side = rect.h - 2.0f * margin;
    minus_ = {rect.x + margin, rect.y + margin, side, side};
    plus_ = {rect.x + rect.w - margin - side, rect.y + margin, side, side};
}

void Stepper::handleInput(const Input &input)
{
    if (minusClick_.update(minus_, input, enabled_ && value_ > min_))
        change(-step_);
    if (plusClick_.update(plus_, input, enabled_ && value_ < max_))
        change(step_);
}

void Stepper::draw(DrawList &list, const Theme &theme) const
{
    list.add(theme.surface(rect_, Shape::Pill, Elevation::Inset, 0.6f));

    auto button = [&](const Rect &r, const ClickState &click, Icon icon, bool available) {
        if (click.held())
            list.add(theme.surface(r, Shape::Circle, Elevation::Inset, 0.4f));
        else
            list.add(theme.surface(r, Shape::Circle, Elevation::Raised, available ? (click.hovered ? 0.6f : 0.45f) : 0.2f,
                                   Finish::Convex));
        drawIcon(list, theme, icon, square(r.center(), r.w * 0.45f), TextTone::Primary, enabled_ && available);
    };
    button(minus_, minusClick_, Icon::Minus, value_ > min_);
    button(plus_, plusClick_, Icon::Plus, value_ < max_);

    const Rect middle{minus_.x + minus_.w, rect_.y, plus_.x - minus_.x - minus_.w, rect_.h};
    list.add(theme.text(format(value_), middle, size_, TextAlign::Center, enabled_));
}

// Knob

Knob::Knob(float value, std::function<void(float)> onChange)
    : value_(std::clamp(value, 0.0f, 1.0f)), onChange_(std::move(onChange))
{
}

Knob &Knob::onChange(std::function<void(float)> callback)
{
    onChange_ = std::move(callback);
    return *this;
}

Knob &Knob::setValue(float value)
{
    value_ = std::clamp(value, 0.0f, 1.0f);
    return *this;
}

Knob &Knob::setDiameter(float pixels)
{
    diameter_ = std::max(48.0f, pixels);
    return *this;
}

Knob &Knob::setFormatter(std::function<std::string(float)> format)
{
    format_ = std::move(format);
    return *this;
}

Vec2 Knob::measure(const Theme &) const
{
    return {diameter_, diameter_};
}

void Knob::handleInput(const Input &input)
{
    if (!enabled_) {
        hovered_ = dragging_ = false;
        return;
    }
    const Vec2 c = rect_.center();
    const float dx = input.mouse.x - c.x;
    const float dy = input.mouse.y - c.y;
    hovered_ = dx * dx + dy * dy <= diameter_ * diameter_ * 0.25f;
    if (input.pressed && hovered_) {
        dragging_ = true;
        dragStartY_ = input.mouse.y;
        dragStartValue_ = value_;
    }
    if (!input.down)
        dragging_ = false;

    // Dragging up turns it up; 200 pixels covers the whole range
    if (dragging_) {
        const float value = std::clamp(dragStartValue_ + (dragStartY_ - input.mouse.y) / 200.0f, 0.0f, 1.0f);
        if (value != value_) {
            value_ = value;
            if (onChange_)
                onChange_(value_);
        }
    }
}

void Knob::draw(DrawList &list, const Theme &theme) const
{
    // The value, the state, the size and the text the formatter gives (its function can't be
    // hashed, so its output is what goes in)
    Fingerprint key;
    key.add(value_);
    key.add(diameter_);
    key.add(hovered_);
    key.add(dragging_);
    key.add(enabled_);
    key.add(static_cast<bool>(format_));
    key.add(format_ ? format_(value_) : std::string());
    cache_.draw(list, theme, rect_, key.value(), [&] {
    const Rect r = square(rect_.center(), std::min(rect_.w, rect_.h));
    const Vec2 c = r.center();
    const float d = r.w;

    list.add(theme.surface(r, Shape::Circle, Elevation::Inset, 0.7f));

    // A ring of dots sweeping 270 degrees clockwise from the bottom left; lit up to the value
    constexpr int kDots = 25;
    const float ringRadius = d * 0.43f;
    const float dot = std::max(3.0f, d * 0.03f);
    for (int i = 0; i < kDots; ++i) {
        const float t = static_cast<float>(i) / (kDots - 1);
        const float angle = (135.0f + 270.0f * t) * kPi / 180.0f;
        const Vec2 p{c.x + std::cos(angle) * ringRadius, c.y + std::sin(angle) * ringRadius};
        if (t <= value_ + 1e-4f)
            list.add(theme.accent(square(p, dot * 1.25f), Shape::Circle, enabled_ ? 1.0f : 0.4f));
        else
            list.add(theme.ink(square(p, dot), Shape::Circle, TextTone::Muted, enabled_));
    }

    // The dial itself, with an accent pointer
    const float body = d * 0.66f;
    const float depth = !enabled_ ? 0.3f : (dragging_ || hovered_ ? 1.0f : 0.8f);
    list.add(theme.surface(square(c, body), Shape::Circle, Elevation::Raised, depth, Finish::Convex));
    const float angle = (135.0f + 270.0f * value_) * kPi / 180.0f;
    const Vec2 pointer{c.x + std::cos(angle) * body * 0.39f, c.y + std::sin(angle) * body * 0.39f};
    list.add(theme.accent(square(pointer, body * 0.08f), Shape::Circle, enabled_ ? 1.0f : 0.4f));

    if (format_) {
        const float px = theme.fontSize(d >= 180.0f ? TextStyle::Title : TextStyle::Body);
        list.add(theme.text(format_(value_), square(c, body * 0.6f), px, TextAlign::Center,
                            enabled_ ? theme.textColor() : theme.mutedTextColor()));
    }
    });
}

// ProgressBar

ProgressBar::ProgressBar(float value)
    : value_(std::clamp(value, 0.0f, 1.0f)), shown_(value_)
{
}

ProgressBar &ProgressBar::setValue(float value)
{
    value_ = std::clamp(value, 0.0f, 1.0f);
    return *this;
}

ProgressBar &ProgressBar::setSize(Size size)
{
    size_ = size;
    return *this;
}

Vec2 ProgressBar::measure(const Theme &theme) const
{
    const float height = theme.controlHeight(size_) * 0.3f;
    return {height * 16.0f, height};
}

void ProgressBar::handleInput(const Input &input)
{
    shown_ += (value_ - shown_) * input.ease(8.0f);
}

void ProgressBar::draw(DrawList &list, const Theme &theme) const
{
    list.add(theme.surface(rect_, Shape::Pill, Elevation::Inset, 0.4f));
    const float inset = rect_.h * 0.2f;
    const Rect inner = rect_.inset(inset);
    const float width = std::max(inner.h, inner.w * shown_);
    if (shown_ > 0.005f)
        list.add(theme.accent({inner.x, inner.y, width, inner.h}, Shape::Pill, enabled_ ? 0.9f : 0.35f));
}

// TextField

namespace {

bool isWordChar(char c)
{
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
}

}

TextField::TextField(std::string placeholder)
    : placeholder_(std::move(placeholder))
{
}

TextField &TextField::setText(std::string text)
{
    text_ = std::move(text);
    caret_ = anchor_ = text_.size();
    return *this;
}

TextField &TextField::setPlaceholder(std::string placeholder)
{
    placeholder_ = std::move(placeholder);
    return *this;
}

TextField &TextField::setPassword(bool password)
{
    password_ = password;
    return *this;
}

TextField &TextField::setSize(Size size)
{
    size_ = size;
    return *this;
}

TextField &TextField::onChange(std::function<void(const std::string &)> callback)
{
    onChange_ = std::move(callback);
    return *this;
}

TextField &TextField::onSubmit(std::function<void(const std::string &)> callback)
{
    onSubmit_ = std::move(callback);
    return *this;
}

bool TextField::focused() const
{
    return hooks_.signals() && hooks_.signals()->focus() == this;
}

void TextField::focus()
{
    if (hooks_.signals()) {
        hooks_.signals()->setFocus(this);
        focusTime_ = time_;
    }
}

void TextField::selectAll()
{
    anchor_ = 0;
    caret_ = text_.size();
}

std::string TextField::shown() const
{
    return password_ ? std::string(text_.size(), '*') : text_;
}

Vec2 TextField::measure(const Theme &theme) const
{
    const float height = theme.controlHeight(size_);
    return {height * 5.0f, height};
}

Rect TextField::textBox() const
{
    const float padX = rect_.h * 0.4f;
    return {rect_.x + padX, rect_.y, std::max(0.0f, rect_.w - 2.0f * padX), rect_.h};
}

void TextField::arrange(const Rect &rect, const Theme &theme)
{
    rect_ = rect;
    theme_ = &theme;
    // Scroll so the caret stays in view, and no further than needed
    const std::string all = shown();
    const float px = theme.fontSize(size_);
    const float width = textBox().w;
    caret_ = std::min(caret_, text_.size());
    anchor_ = std::min(anchor_, text_.size());
    scroll_ = std::min(scroll_, caret_);
    while (scroll_ < caret_ && theme.measureText(all.substr(scroll_, caret_ - scroll_), px).x > width)
        ++scroll_;
    while (scroll_ > 0 && theme.measureText(all.substr(scroll_ - 1), px).x <= width)
        --scroll_;
}

size_t TextField::indexAt(float x) const
{
    if (!theme_)
        return text_.size();
    const std::string all = shown();
    const float px = theme_->fontSize(size_);
    const float left = textBox().x;
    // The boundary nearest x
    float previous = 0.0f;
    for (size_t i = scroll_; i < all.size(); ++i) {
        const float next = theme_->measureText(all.substr(scroll_, i + 1 - scroll_), px).x;
        if (x < left + (previous + next) * 0.5f)
            return i;
        previous = next;
    }
    return all.size();
}

size_t TextField::wordStart(size_t from) const
{
    size_t i = from;
    while (i > 0 && !isWordChar(text_[i - 1]))
        --i;
    while (i > 0 && isWordChar(text_[i - 1]))
        --i;
    return i;
}

size_t TextField::wordEnd(size_t from) const
{
    size_t i = from;
    while (i < text_.size() && !isWordChar(text_[i]))
        ++i;
    while (i < text_.size() && isWordChar(text_[i]))
        ++i;
    return i;
}

void TextField::moveCaret(size_t to, bool select)
{
    caret_ = std::min(to, text_.size());
    if (!select)
        anchor_ = caret_;
    focusTime_ = time_; // the caret stays solid while it moves
}

bool TextField::eraseSelection()
{
    if (anchor_ == caret_)
        return false;
    const size_t from = selectionStart();
    text_.erase(from, selectionEnd() - from);
    caret_ = anchor_ = from;
    return true;
}

void TextField::insert(const std::string &typed)
{
    // The font covers printable ASCII, so only that is accepted
    std::string clean;
    for (char c : typed) {
        if (c >= 32 && c < 127)
            clean += c;
    }
    if (clean.empty())
        return;
    eraseSelection();
    text_.insert(caret_, clean);
    caret_ += clean.size();
    anchor_ = caret_;
    changed();
}

void TextField::changed()
{
    focusTime_ = time_;
    if (onChange_)
        onChange_(text_);
}

bool TextField::onKey(const KeyEvent &e)
{
    if (!focused() || !enabled_)
        return false;
    const bool ctrl = (e.mods & engine::ModCtrl) != 0;
    const bool shift = (e.mods & engine::ModShift) != 0;
    const bool other = (e.mods & (engine::ModAlt | engine::ModSuper)) != 0;
    if (!e.down)
        return !ctrl && !other && !engine::isModifier(e.key);

    switch (e.key) {
    case Key::Left:
        if (!shift && anchor_ != caret_ && !ctrl)
            moveCaret(selectionStart(), false);
        else
            moveCaret(ctrl ? wordStart(caret_) : (caret_ > 0 ? caret_ - 1 : 0), shift);
        return true;
    case Key::Right:
        if (!shift && anchor_ != caret_ && !ctrl)
            moveCaret(selectionEnd(), false);
        else
            moveCaret(ctrl ? wordEnd(caret_) : caret_ + 1, shift);
        return true;
    case Key::Home:
        moveCaret(0, shift);
        return true;
    case Key::End:
        moveCaret(text_.size(), shift);
        return true;
    case Key::Backspace:
        if (!eraseSelection()) {
            if (caret_ == 0)
                return true;
            const size_t from = ctrl ? wordStart(caret_) : caret_ - 1;
            text_.erase(from, caret_ - from);
            caret_ = anchor_ = from;
        }
        changed();
        return true;
    case Key::Delete:
        if (!eraseSelection()) {
            if (caret_ >= text_.size())
                return true;
            const size_t to = ctrl ? wordEnd(caret_) : caret_ + 1;
            text_.erase(caret_, to - caret_);
        }
        changed();
        return true;
    case Key::Enter:
        if (ctrl || other)
            return false;
        if (onSubmit_)
            onSubmit_(text_);
        return true;
    case Key::Escape:
        hooks_.signals()->setFocus(nullptr);
        return true;
    default:
        break;
    }
    // Other shortcuts go on to the app. Plain keys (letters, Space, digits) are typing, so they
    // stop here and never run an action; Tab and the function keys go on.
    if (ctrl || other)
        return false;
    if (e.key == Key::Tab || (e.key >= Key::F1 && e.key <= Key::F12) || engine::isModifier(e.key))
        return false;
    return true;
}

void TextField::makeHooks()
{
    hooks_.key(Priority::Focus, [this](const KeyEvent &e) { return onKey(e); });
    hooks_.text(Priority::Focus, [this](const TextEvent &e) {
        if (!focused() || !enabled_)
            return false;
        if (e.codepoint < 128)
            insert(std::string(1, static_cast<char>(e.codepoint)));
        return true; // other characters aren't in the font: dropped, but still typing
    });
    hooks_.action("edit.selectAll", Priority::Focus, [this](const ActionEvent &) {
        if (!focused())
            return false;
        selectAll();
        return true;
    });
    hooks_.action("edit.copy", Priority::Focus, [this](const ActionEvent &) {
        if (!focused())
            return false;
        if (!password_ && anchor_ != caret_)
            hooks_.signals()->copy(text_.substr(selectionStart(), selectionEnd() - selectionStart()));
        return true;
    });
    hooks_.action("edit.cut", Priority::Focus, [this](const ActionEvent &) {
        if (!focused())
            return false;
        if (!password_ && anchor_ != caret_) {
            hooks_.signals()->copy(text_.substr(selectionStart(), selectionEnd() - selectionStart()));
            eraseSelection();
            changed();
        }
        return true;
    });
    hooks_.action("edit.paste", Priority::Focus, [this](const ActionEvent &) {
        if (!focused())
            return false;
        // One line: newlines become spaces
        std::string pasted = hooks_.signals()->clipboard();
        for (char &c : pasted) {
            if (c == '\n' || c == '\r' || c == '\t')
                c = ' ';
        }
        insert(pasted);
        return true;
    });
}

void TextField::handleInput(const Input &input)
{
    time_ = input.time;
    if (hooks_.attach(input.signals))
        makeHooks();
    InputSignals *signals = hooks_.signals();
    if (!enabled_) {
        if (focused())
            signals->setFocus(nullptr);
        hovered_ = dragging_ = false;
        return;
    }
    hovered_ = rect_.contains(input.mouse);
    if (input.pressed && signals) {
        if (hovered_) {
            const size_t at = indexAt(input.mouse.x);
            const bool doubleClick = input.time - lastPress_ < 0.35;
            lastPress_ = input.time;
            if (!focused()) {
                signals->setFocus(this);
                focusTime_ = input.time;
            }
            if (doubleClick && !password_) {
                // The word under the click
                size_t from = at, to = at;
                while (from > 0 && isWordChar(text_[from - 1]))
                    --from;
                while (to < text_.size() && isWordChar(text_[to]))
                    ++to;
                anchor_ = from;
                caret_ = to;
            } else {
                moveCaret(at, input.shift);
                dragging_ = true;
            }
        } else if (focused()) {
            signals->setFocus(nullptr);
        }
    }
    if (dragging_) {
        if (input.down)
            caret_ = indexAt(input.mouse.x);
        else
            dragging_ = false;
    }
}

void TextField::draw(DrawList &list, const Theme &theme) const
{
    const bool focus = focused();
    list.add(theme.surface(rect_, Shape::Rounded, Elevation::Inset, focus ? 0.9f : (hovered_ ? 0.75f : 0.65f)));

    const float px = theme.fontSize(size_);
    const Rect box = textBox();
    const bool caretOn = focus && std::fmod(time_ - focusTime_, 1.0) < 0.6;

    if (text_.empty()) {
        if (!placeholder_.empty())
            list.add(theme.text(placeholder_, box, px, TextAlign::Start, theme.textColor(TextTone::Muted)));
        if (caretOn)
            list.add(theme.accent({box.x, rect_.y + rect_.h * 0.28f, 2.0f, rect_.h * 0.44f}, Shape::Pill));
        return;
    }

    const std::string all = shown();
    const size_t start = std::min(scroll_, all.size());
    auto xOf = [&](size_t index) {
        index = std::clamp(index, start, all.size());
        return box.x + theme.measureText(all.substr(start, index - start), px).x;
    };
    list.pushClip(box);
    if (focus && anchor_ != caret_) {
        const float from = xOf(selectionStart());
        const float to = std::min(xOf(selectionEnd()), box.x + box.w);
        if (to > from)
            list.add(theme.accent({from, rect_.y + rect_.h * 0.22f, to - from, rect_.h * 0.56f}, Shape::Soft, 0.28f));
    }
    std::string visible = all.substr(start);
    while (!visible.empty() && theme.measureText(visible, px).x > box.w + px)
        visible.pop_back();
    list.add(theme.text(visible, box, px, TextAlign::Start, enabled_ ? theme.textColor() : theme.mutedTextColor()));
    list.popClip();
    if (caretOn)
        list.add(theme.accent({xOf(caret_), rect_.y + rect_.h * 0.28f, 2.0f, rect_.h * 0.44f}, Shape::Pill));
}

// Chip

Chip::Chip(std::string label, bool selected, std::function<void(bool)> onChange)
    : label_(std::move(label)), onChange_(std::move(onChange)), selected_(selected)
{
}

Chip &Chip::onChange(std::function<void(bool)> callback)
{
    onChange_ = std::move(callback);
    return *this;
}

Chip &Chip::setSelected(bool selected)
{
    selected_ = selected;
    return *this;
}

Chip &Chip::setSelectable(bool selectable)
{
    selectable_ = selectable;
    return *this;
}

Vec2 Chip::measure(const Theme &theme) const
{
    const float height = theme.controlHeight(Size::Small) * 0.85f;
    return {theme.measureText(label_, Size::Small).x + height * 0.9f, height};
}

void Chip::handleInput(const Input &input)
{
    if (click_.update(rect_, input, enabled_ && selectable_)) {
        selected_ = !selected_;
        if (onChange_)
            onChange_(selected_);
    }
}

void Chip::draw(DrawList &list, const Theme &theme) const
{
    TextTone tone = TextTone::Primary;
    if (!selectable_) {
        list.add(theme.surface(rect_, Shape::Pill, Elevation::Inset, 0.35f));
        tone = TextTone::Secondary;
    } else if (selected_) {
        list.add(theme.surface(rect_, Shape::Pill, Elevation::Inset, 0.5f));
        tone = TextTone::Accent;
    } else {
        list.add(theme.surface(rect_, Shape::Pill, Elevation::Raised, click_.hovered ? 0.6f : 0.45f));
    }
    list.add(theme.text(label_, rect_, theme.fontSize(Size::Small), TextAlign::Center,
                        enabled_ ? theme.textColor(tone) : theme.mutedTextColor()));
}

// Badge

Badge::Badge(std::string text)
    : text_(std::move(text))
{
}

Badge &Badge::setText(std::string text)
{
    text_ = std::move(text);
    return *this;
}

Vec2 Badge::measure(const Theme &theme) const
{
    const float px = theme.fontSize(TextStyle::Caption);
    const float height = std::round(px * 1.7f);
    return {std::max(height, theme.measureText(text_, px).x + height * 0.8f), height};
}

void Badge::draw(DrawList &list, const Theme &theme) const
{
    list.add(theme.accent(rect_, Shape::Pill, enabled_ ? 1.0f : 0.4f));
    list.add(theme.text(text_, rect_, TextStyle::Caption, TextTone::OnAccent, TextAlign::Center));
}

// LevelMeter

LevelMeter::LevelMeter(Direction direction)
    : direction_(direction)
{
}

LevelMeter &LevelMeter::setLevel(float amplitude)
{
    level_ = std::max(0.0f, amplitude);
    return *this;
}

LevelMeter &LevelMeter::setRange(float minDb, float maxDb)
{
    minDb_ = std::min(minDb, maxDb - 1.0f);
    maxDb_ = maxDb;
    return *this;
}

LevelMeter &LevelMeter::setThickness(float pixels)
{
    thickness_ = std::max(4.0f, pixels);
    return *this;
}

LevelMeter &LevelMeter::setLength(float pixels)
{
    length_ = std::max(thickness_, pixels);
    return *this;
}

float LevelMeter::position(float amplitude) const
{
    const float db = amplitude > 1e-6f ? 20.0f * std::log10(amplitude) : -120.0f;
    return std::clamp((db - minDb_) / (maxDb_ - minDb_), 0.0f, 1.0f);
}

Vec2 LevelMeter::measure(const Theme &) const
{
    return direction_ == Direction::Row ? Vec2{length_, thickness_} : Vec2{thickness_, length_};
}

void LevelMeter::handleInput(const Input &input)
{
    // Rises at once; falls about 20 dB a second, like a meter's ballistics
    const float target = position(level_);
    const float fall = 20.0f / (maxDb_ - minDb_) * input.dt;
    shown_ = target >= shown_ ? target : std::max(target, shown_ - fall);

    // The hold waits a second, then falls the same way
    holdAge_ += input.dt;
    if (target >= hold_) {
        hold_ = target;
        holdAge_ = 0.0f;
    } else if (holdAge_ > 1.0f) {
        hold_ = std::max(shown_, hold_ - fall);
    }

    clipAge_ = level_ >= 1.0f ? 0.0f : clipAge_ + input.dt;
}

void LevelMeter::draw(DrawList &list, const Theme &theme) const
{
    const bool row = direction_ == Direction::Row;
    list.add(theme.surface(rect_, Shape::Pill, Elevation::Inset, 0.4f));

    const float inset = std::min(rect_.w, rect_.h) * 0.22f;
    const Rect inner = rect_.inset(inset);
    const float length = row ? inner.w : inner.h;
    const float cross = row ? inner.h : inner.w;
    // Clipped recently: the whole track lights up for a moment
    const bool clipping = clipAge_ < 0.6f;
    const float filled = clipping ? 1.0f : shown_;
    if (filled > 0.001f) {
        const float extent = std::max(cross, length * filled);
        // Columns fill from the bottom up
        const Rect fill = row ? Rect{inner.x, inner.y, extent, inner.h}
                              : Rect{inner.x, inner.y + inner.h - extent, inner.w, extent};
        list.add(theme.accent(fill, Shape::Pill, clipping ? 1.0f : 0.85f));
    }
    if (hold_ > 0.01f && !clipping) {
        const float at = length * hold_;
        const float mark = std::max(2.0f, cross * 0.35f);
        const Rect marker = row ? Rect{inner.x + std::max(0.0f, at - mark), inner.y, mark, inner.h}
                                : Rect{inner.x, inner.y + inner.h - std::max(mark, at), inner.w, mark};
        list.add(theme.ink(marker, Shape::Pill, TextTone::Primary, enabled_));
    }
}

// Avatar

Avatar::Avatar(std::string initials, Size size)
    : initials_(std::move(initials)), size_(size)
{
}

Vec2 Avatar::measure(const Theme &theme) const
{
    const float d = theme.controlHeight(size_);
    return {d, d};
}

void Avatar::draw(DrawList &list, const Theme &theme) const
{
    const Rect r = square(rect_.center(), std::min(rect_.w, rect_.h));
    list.add(theme.surface(r, Shape::Circle, Elevation::Raised, 0.6f, Finish::Convex));
    list.add(theme.text(initials_, r, theme.fontSize(size_), TextAlign::Center, theme.textColor(TextTone::Accent)));
}

}
