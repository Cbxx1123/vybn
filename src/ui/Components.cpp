#include "Components.h"

#include <algorithm>
#include <sstream>
#include <utility>

namespace ui {

namespace {

// Largest square centred in `rect`
Rect centredSquare(const Rect &rect)
{
    float side = std::min(rect.w, rect.h);
    return {rect.x + (rect.w - side) * 0.5f, rect.y + (rect.h - side) * 0.5f, side, side};
}

// Diameter of the dot marking an Accent button
float accentDotSize(float buttonHeight)
{
    return buttonHeight * 0.22f;
}

// Icons are drawn a little larger than the text beside them
float iconSize(const Theme &theme, Size size)
{
    return theme.fontSize(size) * 1.25f;
}

// Pressing a convex surface makes it concave, and the reverse
Finish pressedFinish(Finish finish)
{
    if (finish == Finish::Convex)
        return Finish::Concave;
    if (finish == Finish::Concave)
        return Finish::Convex;
    return finish;
}

}

// Icons

void drawIcon(DrawList &list, const Theme &theme, Icon icon, const Rect &box, TextTone tone, bool enabled)
{
    const Rect sq = centredSquare(box);
    const float s = sq.w;
    const Vec2 c = sq.center();
    auto ink = [&](float cx, float cy, float w, float h, Shape shape) {
        list.add(theme.ink({cx - w * 0.5f, cy - h * 0.5f, w, h}, shape, tone, enabled));
    };
    const float stroke = std::max(2.0f, s * 0.11f);
    const float thin = std::max(1.5f, s * 0.08f);
    // A square outline of side `side` from four thin strokes
    auto outline = [&](float cx, float cy, float side) {
        ink(cx, cy - (side - thin) * 0.5f, side, thin, Shape::Pill);
        ink(cx, cy + (side - thin) * 0.5f, side, thin, Shape::Pill);
        ink(cx - (side - thin) * 0.5f, cy, thin, side, Shape::Pill);
        ink(cx + (side - thin) * 0.5f, cy, thin, side, Shape::Pill);
    };

    switch (icon) {
    case Icon::None:
        break;
    case Icon::Menu:
        for (int i = -1; i <= 1; ++i)
            ink(c.x, c.y + i * s * 0.24f, s * 0.68f, stroke, Shape::Pill);
        break;
    case Icon::Plus:
        ink(c.x, c.y, s * 0.62f, stroke, Shape::Pill);
        ink(c.x, c.y, stroke, s * 0.62f, Shape::Pill);
        break;
    case Icon::Minus:
        ink(c.x, c.y, s * 0.62f, stroke, Shape::Pill);
        break;
    case Icon::Play:
        ink(c.x + s * 0.05f, c.y, s * 0.5f, s * 0.58f, Shape::TriangleRight);
        break;
    case Icon::Pause:
        ink(c.x - s * 0.13f, c.y, s * 0.16f, s * 0.56f, Shape::Soft);
        ink(c.x + s * 0.13f, c.y, s * 0.16f, s * 0.56f, Shape::Soft);
        break;
    case Icon::Next:
        ink(c.x - s * 0.06f, c.y, s * 0.4f, s * 0.48f, Shape::TriangleRight);
        ink(c.x + s * 0.21f, c.y, stroke, s * 0.48f, Shape::Soft);
        break;
    case Icon::Previous:
        ink(c.x + s * 0.06f, c.y, s * 0.4f, s * 0.48f, Shape::TriangleLeft);
        ink(c.x - s * 0.21f, c.y, stroke, s * 0.48f, Shape::Soft);
        break;
    case Icon::Stop: {
        // A square, only slightly rounded: the Soft radius would make a small one a dot
        const float side = s * 0.46f;
        Surface square = theme.ink({c.x - side * 0.5f, c.y - side * 0.5f, side, side}, Shape::Rounded, tone, enabled);
        square.radius = side * 0.18f;
        list.add(square);
        break;
    }
    case Icon::Dot:
        ink(c.x, c.y, s * 0.3f, s * 0.3f, Shape::Circle);
        break;
    case Icon::Grid:
        for (int i = 0; i < 4; ++i)
            ink(c.x + (i % 2 ? 1 : -1) * s * 0.16f, c.y + (i / 2 ? 1 : -1) * s * 0.16f, s * 0.26f, s * 0.26f, Shape::Soft);
        break;
    case Icon::List:
        for (int i = -1; i <= 1; ++i) {
            ink(c.x - s * 0.26f, c.y + i * s * 0.24f, stroke * 1.2f, stroke * 1.2f, Shape::Circle);
            ink(c.x + s * 0.08f, c.y + i * s * 0.24f, s * 0.46f, stroke, Shape::Pill);
        }
        break;
    case Icon::Chevron:
        ink(c.x + s * 0.04f, c.y, s * 0.36f, s * 0.5f, Shape::TriangleRight);
        break;
    case Icon::ChevronDown:
        ink(c.x, c.y + s * 0.04f, s * 0.5f, s * 0.36f, Shape::TriangleDown);
        break;
    case Icon::Up:
        // An arrow: a head over a stem
        ink(c.x, c.y - s * 0.12f, s * 0.5f, s * 0.3f, Shape::TriangleUp);
        ink(c.x, c.y + s * 0.13f, stroke, s * 0.3f, Shape::Pill);
        break;
    case Icon::File:
        // A page with lines on it
        outline(c.x, c.y, s * 0.52f);
        ink(c.x, c.y - s * 0.06f, s * 0.24f, thin, Shape::Pill);
        ink(c.x, c.y + s * 0.08f, s * 0.24f, thin, Shape::Pill);
        break;
    case Icon::Search:
        // A ring with a handle
        ink(c.x - s * 0.07f, c.y - s * 0.07f, s * 0.42f, s * 0.42f, Shape::Circle);
        {
            Surface hole = theme.ink({c.x - s * 0.07f - s * 0.13f, c.y - s * 0.07f - s * 0.13f, s * 0.26f, s * 0.26f},
                                     Shape::Circle, tone, enabled);
            hole.base = theme.baseColor();
            list.add(hole);
        }
        ink(c.x + s * 0.2f, c.y + s * 0.2f, s * 0.14f, s * 0.14f, Shape::Circle);
        break;
    case Icon::PopOut:
        // A window with a smaller one lifting off its top-right corner
        outline(c.x - s * 0.06f, c.y + s * 0.06f, s * 0.42f);
        ink(c.x + s * 0.17f, c.y - s * 0.17f, s * 0.24f, s * 0.24f, Shape::Soft);
        break;
    case Icon::Dock:
        // A window with a panel set into its lower half
        outline(c.x, c.y, s * 0.5f);
        ink(c.x, c.y + s * 0.1f, s * 0.36f, s * 0.14f, Shape::Soft);
        break;
    case Icon::Check:
        ink(c.x, c.y, s * 0.34f, s * 0.34f, Shape::Circle);
        break;
    case Icon::Close:
        ink(c.x, c.y, s * 0.62f, s * 0.62f, Shape::Cross);
        break;
    case Icon::Maximize:
        outline(c.x, c.y, s * 0.5f);
        break;
    case Icon::Restore:
        // Two overlapping windows: the back one shows only its top and right edges
        outline(c.x - s * 0.06f, c.y + s * 0.06f, s * 0.4f);
        ink(c.x + s * 0.06f, c.y - s * 0.26f + thin * 0.5f, s * 0.4f, thin, Shape::Pill);
        ink(c.x + s * 0.26f - thin * 0.5f, c.y - s * 0.06f, thin, s * 0.4f, Shape::Pill);
        break;
    case Icon::Download:
        // An arrow down onto a tray
        ink(c.x, c.y - s * 0.12f, stroke, s * 0.36f, Shape::Pill);
        ink(c.x, c.y + s * 0.08f, s * 0.36f, s * 0.24f, Shape::TriangleDown);
        ink(c.x, c.y + s * 0.3f, s * 0.6f, stroke, Shape::Pill);
        break;
    case Icon::Folder:
        // A tab over a body
        ink(c.x - s * 0.15f, c.y - s * 0.19f, s * 0.26f, s * 0.14f, Shape::Soft);
        ink(c.x, c.y + s * 0.05f, s * 0.62f, s * 0.42f, Shape::Soft);
        break;
    case Icon::Wave: {
        // Bars of a waveform
        const float heights[] = {0.2f, 0.44f, 0.62f, 0.34f, 0.5f, 0.24f};
        for (int i = 0; i < 6; ++i)
            ink(c.x + (i - 2.5f) * s * 0.11f, c.y, std::max(1.5f, s * 0.07f), s * heights[i], Shape::Pill);
        break;
    }
    case Icon::Gear: {
        // Teeth around a ring
        for (int i = 0; i < 8; ++i) {
            const float a = i * 3.14159265f / 4.0f;
            ink(c.x + std::cos(a) * s * 0.29f, c.y + std::sin(a) * s * 0.29f, s * 0.16f, s * 0.16f, Shape::Circle);
        }
        ink(c.x, c.y, s * 0.52f, s * 0.52f, Shape::Circle);
        Surface hole = theme.ink({c.x - s * 0.11f, c.y - s * 0.11f, s * 0.22f, s * 0.22f}, Shape::Circle, tone, enabled);
        hole.base = theme.baseColor();
        list.add(hole);
        break;
    }
    case Icon::Sliders:
        // Three tracks, each with a knob at a different place
        for (int i = -1; i <= 1; ++i) {
            ink(c.x, c.y + i * s * 0.24f, s * 0.62f, thin, Shape::Pill);
            ink(c.x + (i == 0 ? 0.14f : i < 0 ? -0.16f : 0.06f) * s, c.y + i * s * 0.24f, s * 0.16f, s * 0.16f, Shape::Circle);
        }
        break;
    case Icon::Keys:
        // Three white keys with two black keys between them
        outline(c.x, c.y, s * 0.6f);
        ink(c.x - s * 0.1f, c.y + s * 0.02f, thin, s * 0.56f, Shape::Pill);
        ink(c.x + s * 0.1f, c.y + s * 0.02f, thin, s * 0.56f, Shape::Pill);
        ink(c.x - s * 0.1f, c.y - s * 0.1f, s * 0.1f, s * 0.32f, Shape::Soft);
        ink(c.x + s * 0.1f, c.y - s * 0.1f, s * 0.1f, s * 0.32f, Shape::Soft);
        break;
    }
}

// Button

Button::Button(std::function<void()> onClick)
    : onClick_(std::move(onClick))
{
}

Button &Button::onClick(std::function<void()> callback)
{
    onClick_ = std::move(callback);
    return *this;
}

Button &Button::setVariant(ButtonVariant variant)
{
    variant_ = variant;
    return *this;
}

Button &Button::setShape(Shape shape)
{
    shape_ = shape;
    return *this;
}

Button &Button::setSize(Size size)
{
    size_ = size;
    return *this;
}

Button &Button::setFinish(Finish finish)
{
    finish_ = finish;
    return *this;
}

Button &Button::setDepth(Depth depth)
{
    depth_ = depth;
    return *this;
}

Button &Button::setLabel(std::string label)
{
    label_ = std::move(label);
    return *this;
}

Button &Button::setIcon(Icon icon)
{
    icon_ = icon;
    return *this;
}

Button &Button::setTone(TextTone tone)
{
    tone_ = tone;
    return *this;
}

Button &Button::setSelected(bool selected)
{
    selected_ = selected;
    return *this;
}

Vec2 Button::measure(const Theme &theme) const
{
    float height = theme.controlHeight(size_);
    if (shape_ == Shape::Circle || label_.empty())
        return {height, height};

    // Half a control height of padding on each side of the content
    float content = theme.measureText(label_, size_).x;
    if (icon_ != Icon::None)
        content += iconSize(theme, size_) + theme.fontSize(size_) * 0.4f;
    else if (variant_ == ButtonVariant::Accent)
        content += 2.0f * accentDotSize(height);
    return {std::max(height * 2.5f, content + height), height};
}

void Button::handleInput(const Input &input)
{
    if (click_.update(rect_, input, enabled_) && onClick_)
        onClick_();
}

void Button::draw(DrawList &list, const Theme &theme) const
{
    const Rect r = shape_ == Shape::Circle ? centredSquare(rect_) : rect_;
    const float depth = theme.depthScale(depth_);
    const bool down = click_.held() || selected_;

    if (!enabled_)
        list.add(theme.surface(r, shape_, variant_ == ButtonVariant::Inset ? Elevation::Inset : Elevation::Raised, 0.35f));
    else if (variant_ == ButtonVariant::Inset)
        list.add(theme.surface(r, shape_, click_.held() ? Elevation::Flat : Elevation::Inset, 0.7f * depth, finish_));
    else if (down)
        list.add(theme.surface(r, shape_, Elevation::Inset, 0.6f * depth, pressedFinish(finish_)));
    else if (variant_ == ButtonVariant::Flat && !click_.hovered)
        list.add(theme.surface(r, shape_, Elevation::Flat, 1.0f, finish_));
    else
        list.add(theme.surface(r, shape_, Elevation::Raised, depth * (click_.hovered ? 1.25f : 1.0f), finish_));

    // Content is centred: the accent dot or icon, a gap, then the label
    const bool accent = variant_ == ButtonVariant::Accent;
    const float px = theme.fontSize(size_);
    const float lead = icon_ != Icon::None ? iconSize(theme, size_) : accent ? accentDotSize(r.h) : 0.0f;
    const float gap = label_.empty() || lead == 0.0f ? 0.0f : (icon_ != Icon::None ? px * 0.4f : lead);
    const float textWidth = label_.empty() ? 0.0f : theme.measureText(label_, size_).x;
    const float content = lead + gap + textWidth;
    const Vec2 c = r.center();
    const float start = c.x - content * 0.5f;

    // Selected and accent content picks up the accent colour
    const TextTone tone = selected_ ? TextTone::Accent : tone_;
    if (icon_ != Icon::None)
        drawIcon(list, theme, icon_, {start, c.y - lead * 0.5f, lead, lead}, accent ? TextTone::Accent : tone, enabled_);
    else if (accent)
        list.add(theme.accent({start, c.y - lead * 0.5f, lead, lead}, Shape::Circle, enabled_ ? 1.0f : 0.4f));
    if (!label_.empty()) {
        const Rect box{start + lead + gap, r.y, textWidth, r.h};
        list.add(theme.text(label_, box, px, TextAlign::Start, enabled_ ? theme.textColor(tone) : theme.mutedTextColor()));
    }
}

// Toggle

Toggle::Toggle(bool on, std::function<void(bool)> onChange)
    : onChange_(std::move(onChange)), on_(on), knob_(on ? 1.0f : 0.0f)
{
}

Toggle &Toggle::onChange(std::function<void(bool)> callback)
{
    onChange_ = std::move(callback);
    return *this;
}

Toggle &Toggle::setOn(bool on)
{
    on_ = on;
    return *this;
}

Toggle &Toggle::setSize(Size size)
{
    size_ = size;
    return *this;
}

Vec2 Toggle::measure(const Theme &theme) const
{
    float height = theme.controlHeight(size_) * 0.75f;
    return {height * 1.9f, height};
}

void Toggle::handleInput(const Input &input)
{
    // Ease the knob toward its target
    knob_ += ((on_ ? 1.0f : 0.0f) - knob_) * input.ease();

    if (click_.update(rect_, input, enabled_)) {
        on_ = !on_;
        if (onChange_)
            onChange_(on_);
    }
}

void Toggle::draw(DrawList &list, const Theme &theme) const
{
    list.add(theme.surface(rect_, Shape::Pill, Elevation::Inset, 0.6f));
    if (knob_ > 0.01f)
        list.add(theme.accent(rect_.inset(rect_.h * 0.12f), Shape::Pill, 0.55f * knob_ * (enabled_ ? 1.0f : 0.4f)));

    float margin = rect_.h * 0.12f;
    float diameter = rect_.h - 2.0f * margin;
    float travel = rect_.w - 2.0f * margin - diameter;
    Rect knob{rect_.x + margin + travel * knob_, rect_.y + margin, diameter, diameter};
    float depth = !enabled_ ? 0.25f : (click_.hovered ? 0.7f : 0.55f);
    list.add(theme.surface(knob, Shape::Circle, Elevation::Raised, depth, Finish::Convex));
}

// Slider

Slider::Slider(float value, std::function<void(float)> onChange)
    : onChange_(std::move(onChange)), value_(std::clamp(value, 0.0f, 1.0f))
{
}

Slider &Slider::onChange(std::function<void(float)> callback)
{
    onChange_ = std::move(callback);
    return *this;
}

Slider &Slider::setValue(float value)
{
    value_ = std::clamp(value, 0.0f, 1.0f);
    return *this;
}

Slider &Slider::setSize(Size size)
{
    size_ = size;
    return *this;
}

float Slider::knobSize() const
{
    return rect_.h;
}

// The knob centre travels from one knob-radius in from each end, so it never leaves the rect
float Slider::trackStart() const
{
    return rect_.x + knobSize() * 0.5f;
}

float Slider::trackLength() const
{
    return std::max(1.0f, rect_.w - knobSize());
}

Vec2 Slider::measure(const Theme &theme) const
{
    float knob = theme.controlHeight(size_) * 0.6f;
    return {knob * 6.0f, knob};
}

void Slider::handleInput(const Input &input)
{
    if (!enabled_) {
        hovered_ = dragging_ = false;
        return;
    }
    hovered_ = rect_.contains(input.mouse);
    if (input.pressed && hovered_)
        dragging_ = true;
    if (!input.down)
        dragging_ = false;

    if (dragging_) {
        float value = std::clamp((input.mouse.x - trackStart()) / trackLength(), 0.0f, 1.0f);
        if (value != value_) {
            value_ = value;
            if (onChange_)
                onChange_(value_);
        }
    }
}

void Slider::draw(DrawList &list, const Theme &theme) const
{
    const float knob = knobSize();
    const float trackHeight = knob * 0.4f;
    const Rect track{rect_.x, rect_.y + (rect_.h - trackHeight) * 0.5f, rect_.w, trackHeight};
    list.add(theme.surface(track, Shape::Pill, Elevation::Inset, 0.45f));

    const float knobCentre = trackStart() + trackLength() * value_;
    const float fillInset = trackHeight * 0.2f;
    const Rect fill{track.x + fillInset, track.y + fillInset, knobCentre - track.x - fillInset, trackHeight - 2.0f * fillInset};
    if (fill.w > 0.0f)
        list.add(theme.accent(fill, Shape::Pill, enabled_ ? 0.85f : 0.35f));

    const Rect handle{knobCentre - knob * 0.5f, rect_.y, knob, knob};
    float depth = !enabled_ ? 0.25f : (dragging_ || hovered_ ? 0.75f : 0.6f);
    list.add(theme.surface(handle, Shape::Circle, Elevation::Raised, depth, Finish::Convex));
}

// Label

Label::Label(std::string text, Size size)
    : text_(std::move(text)), size_(size)
{
}

Label::Label(std::string text, TextStyle style, TextTone tone)
    : text_(std::move(text)), style_(style), tone_(tone)
{
}

Label &Label::setText(std::string text)
{
    text_ = std::move(text);
    return *this;
}

Label &Label::setSize(Size size)
{
    size_ = size;
    style_.reset();
    return *this;
}

Label &Label::setStyle(TextStyle style)
{
    style_ = style;
    return *this;
}

Label &Label::setTone(TextTone tone)
{
    tone_ = tone;
    return *this;
}

Label &Label::setAlignment(TextAlign align)
{
    align_ = align;
    return *this;
}

Label &Label::setWrapWidth(float width)
{
    wrapWidth_ = std::max(0.0f, width);
    return *this;
}

Label &Label::setMaxWidth(float width)
{
    maxWidth_ = std::max(0.0f, width);
    return *this;
}

float Label::fontSize(const Theme &theme) const
{
    return style_ ? theme.fontSize(*style_) : theme.fontSize(size_);
}

const std::vector<std::string> &Label::lines(const Theme &theme) const
{
    const float px = fontSize(theme);
    if (px == linesPx_ && wrapWidth_ == linesWrap_ && maxWidth_ == linesMax_ && text_ == linesText_)
        return lines_;
    linesPx_ = px;
    linesWrap_ = wrapWidth_;
    linesMax_ = maxWidth_;
    linesText_ = text_;
    lines_.clear();
    std::vector<std::string> &result = lines_;
    if (wrapWidth_ <= 0.0f) {
        std::string line = text_;
        if (maxWidth_ > 0.0f && theme.measureText(line, px).x > maxWidth_) {
            // Drop characters from the end until the line and its "..." fit
            const std::string dots = "...";
            size_t keep = line.size();
            while (keep > 0 && theme.measureText(line.substr(0, keep) + dots, px).x > maxWidth_)
                --keep;
            line = line.substr(0, keep) + dots;
        }
        result.push_back(line);
        linesWidth_ = theme.measureText(line, px).x;
        return result;
    }

    // Greedy word wrap; explicit newlines always break
    std::istringstream paragraphs(text_);
    std::string paragraph;
    while (std::getline(paragraphs, paragraph)) {
        std::istringstream words(paragraph);
        std::string word;
        std::string line;
        while (words >> word) {
            std::string candidate = line.empty() ? word : line + " " + word;
            if (!line.empty() && theme.measureText(candidate, px).x > wrapWidth_) {
                result.push_back(line);
                line = word;
            } else {
                line = candidate;
            }
        }
        result.push_back(line);
    }
    linesWidth_ = 0.0f;
    for (const std::string &l : result)
        linesWidth_ = std::max(linesWidth_, theme.measureText(l, px).x);
    return result;
}

Vec2 Label::measure(const Theme &theme) const
{
    // Empty text takes no room, so a label that's blank for now doesn't leave a gap
    if (text_.empty())
        return {0.0f, 0.0f};
    const float px = fontSize(theme);
    const std::vector<std::string> &all = lines(theme);
    return {linesWidth_, theme.font().lineHeight(px) * static_cast<float>(all.size())};
}

void Label::draw(DrawList &list, const Theme &theme) const
{
    // What the lines derive from: the text, the size or style, the tone, alignment, enabled and
    // the wrap and max widths (the lines and their metrics follow from them and the theme)
    Fingerprint key;
    key.add(text_);
    key.add(size_);
    key.add(style_.has_value());
    key.add(style_.has_value() ? static_cast<int>(*style_) : -1);
    key.add(tone_);
    key.add(align_);
    key.add(enabled_);
    key.add(wrapWidth_);
    key.add(maxWidth_);
    cache_.draw(list, theme, rect_, key.value(), [&] {
        const float px = fontSize(theme);
        const float lineHeight = theme.font().lineHeight(px);
        const Color color = enabled_ ? theme.textColor(tone_) : theme.mutedTextColor();
        const std::vector<std::string> &all = lines(theme);

        // Centre the block of lines vertically in the rect
        float y = rect_.y + (rect_.h - lineHeight * all.size()) * 0.5f;
        for (const std::string &line : all) {
            list.add(theme.text(line, {rect_.x, y, rect_.w, lineHeight}, px, align_, color));
            y += lineHeight;
        }
    });
}

// Panel

Panel::Panel(Direction direction, Elevation elevation)
    : Container(direction), elevation_(elevation)
{
}

Panel &Panel::setElevation(Elevation elevation)
{
    elevation_ = elevation;
    return *this;
}

Panel &Panel::setFinish(Finish finish)
{
    finish_ = finish;
    return *this;
}

Panel &Panel::setDepth(Depth depth)
{
    depth_ = depth;
    return *this;
}

Panel &Panel::setShape(Shape shape)
{
    shape_ = shape == Shape::Soft ? Shape::Soft : Shape::Rounded;
    return *this;
}

Panel &Panel::setClipContent(bool clip)
{
    clip_ = clip;
    return *this;
}

float Panel::padding(const Theme &theme) const
{
    return std::max(theme.padding(), padding_);
}

void Panel::draw(DrawList &list, const Theme &theme) const
{
    list.add(theme.surface(rect_, shape_, elevation_, theme.depthScale(depth_), finish_));
    if (clip_)
        list.pushClip(rect_);
    Container::draw(list, theme);
    if (clip_)
        list.popClip();
}

// Spacer

Spacer::Spacer(float grow)
{
    grow_ = grow;
}

// Divider

Divider::Divider()
{
    alignSelf_ = Align::Stretch;
}

Vec2 Divider::measure(const Theme &) const
{
    return {4.0f, 4.0f};
}

void Divider::draw(DrawList &list, const Theme &theme) const
{
    list.add(theme.surface(rect_, Shape::Pill, Elevation::Inset, 0.25f));
}

}
