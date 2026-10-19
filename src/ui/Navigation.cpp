#include "Navigation.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace ui {

// ListItem

ListItem::ListItem(std::string title, std::function<void()> onClick)
    : title_(std::move(title)), onClick_(std::move(onClick))
{
    alignSelf_ = Align::Stretch;
}

ListItem &ListItem::onClick(std::function<void()> callback)
{
    onClick_ = std::move(callback);
    return *this;
}

ListItem &ListItem::setTitle(std::string title)
{
    title_ = std::move(title);
    return *this;
}

ListItem &ListItem::setSubtitle(std::string subtitle)
{
    subtitle_ = std::move(subtitle);
    return *this;
}

ListItem &ListItem::setTrailing(std::string trailing)
{
    trailing_ = std::move(trailing);
    return *this;
}

ListItem &ListItem::setIcon(Icon icon)
{
    icon_ = icon;
    return *this;
}

ListItem &ListItem::setSelected(bool selected)
{
    selected_ = selected;
    return *this;
}

ListItem &ListItem::setSize(Size size)
{
    size_ = size;
    return *this;
}

Vec2 ListItem::measure(const Theme &theme) const
{
    const float height = theme.controlHeight(size_) * (subtitle_.empty() ? 1.0f : 1.3f);
    const float px = theme.fontSize(size_);
    float width = std::max(theme.measureText(title_, size_).x, theme.measureText(subtitle_, TextStyle::Caption).x);
    if (icon_ != Icon::None)
        width += px * 2.2f;
    if (!trailing_.empty())
        width += theme.measureText(trailing_, TextStyle::Caption).x + px;
    return {width + height * 0.6f, height};
}

void ListItem::handleInput(const Input &input)
{
    if (click_.update(rect_, input, enabled_) && onClick_)
        onClick_();
}

void ListItem::draw(DrawList &list, const Theme &theme) const
{
    if (selected_ || click_.held())
        list.add(theme.surface(rect_, Shape::Rounded, Elevation::Inset, 0.55f));
    else if (click_.hovered)
        list.add(theme.surface(rect_, Shape::Rounded, Elevation::Raised, 0.45f));

    const float px = theme.fontSize(size_);
    const float pad = rect_.h * (subtitle_.empty() ? 0.3f : 0.25f);
    float x = rect_.x + pad;
    const TextTone tone = !enabled_ ? TextTone::Muted : selected_ ? TextTone::Accent : TextTone::Primary;

    if (icon_ != Icon::None) {
        const float icon = px * 1.3f;
        drawIcon(list, theme, icon_, {x, rect_.y + (rect_.h - icon) * 0.5f, icon, icon}, tone, enabled_);
        x += px * 2.2f;
    }

    float right = rect_.x + rect_.w - pad;
    if (!trailing_.empty()) {
        const float w = theme.measureText(trailing_, TextStyle::Caption).x;
        list.add(theme.text(trailing_, {right - w, rect_.y, w, rect_.h}, TextStyle::Caption, TextTone::Secondary));
        right -= w + px;
    }

    const float width = std::max(0.0f, right - x);
    if (subtitle_.empty()) {
        list.add(theme.text(title_, {x, rect_.y, width, rect_.h}, px, TextAlign::Start, theme.textColor(tone)));
    } else {
        // Title and subtitle share the height, nudged toward each other
        const float half = rect_.h * 0.5f;
        list.add(theme.text(title_, {x, rect_.y + half * 0.15f, width, half}, px, TextAlign::Start, theme.textColor(tone)));
        list.add(theme.text(subtitle_, {x, rect_.y + half * 0.85f, width, half}, TextStyle::Caption, TextTone::Secondary));
    }
}

// ListView

ListView::ListView()
    : Container(Direction::Column)
{
    align_ = Align::Stretch;
}

float ListView::gap(const Theme &theme) const
{
    return std::max(theme.spacing() * 0.2f, gap_);
}

// Pages

Pages::Pages()
{
    align_ = Align::Stretch;
}

Pages &Pages::setCurrent(size_t index)
{
    current_ = index;
    return *this;
}

Vec2 Pages::measure(const Theme &theme) const
{
    if (current_ >= children_.size())
        return {};
    return children_[current_]->preferredSize(theme);
}

void Pages::arrange(const Rect &rect, const Theme &theme)
{
    rect_ = rect;
    if (current_ < children_.size())
        children_[current_]->arrange(rect, theme);
}

void Pages::handleInput(const Input &input)
{
    if (current_ < children_.size())
        children_[current_]->handleInput(input);
}

void Pages::draw(DrawList &list, const Theme &theme) const
{
    if (current_ < children_.size())
        children_[current_]->draw(list, theme);
}

// ScrollView

namespace {

constexpr float kWheelStep = 60.0f; // pixels per wheel line
constexpr float kBarWidth = 8.0f;

float along(Vec2 v, int axis)
{
    return axis == 0 ? v.x : v.y;
}

}

ScrollView::ScrollView()
    : content_(std::make_unique<Container>(Direction::Column))
{
    content_->setAlign(Align::Stretch);
}

std::unique_ptr<Container> ScrollView::takeContent()
{
    std::unique_ptr<Container> taken = std::move(content_);
    content_ = std::make_unique<Container>(Direction::Column);
    content_->setAlign(Align::Stretch);
    Component::treeChanged();
    scrollTo(0.0f);
    return taken;
}

void ScrollView::setContent(std::unique_ptr<Container> content)
{
    if (content) {
        content_ = std::move(content);
        Component::treeChanged();
    }
    scrollTo(0.0f);
}

ScrollView &ScrollView::setViewHeight(float height)
{
    viewHeight_ = std::max(0.0f, height);
    return *this;
}

ScrollView &ScrollView::scrollTo(float y, float x)
{
    target_[0] = offset_[0] = std::max(0.0f, x);
    target_[1] = offset_[1] = std::max(0.0f, y);
    return *this;
}

Vec2 ScrollView::measure(const Theme &theme) const
{
    // Wide enough to show the content without scrolling across, if the parent allows
    const float inset = theme.shadowExtent();
    return {content_->preferredSize(theme).x + 2.0f * inset + kBarWidth + inset, viewHeight_};
}

void ScrollView::arrange(const Rect &rect, const Theme &theme)
{
    rect_ = rect;
    inset_ = theme.shadowExtent();
    const Vec2 wanted = content_->preferredSize(theme);
    const float bar = kBarWidth + inset_; // room a scrollbar takes from the view

    // Each scrollbar takes room from the other axis, so check vertical, then horizontal, then
    // vertical again in case the horizontal bar made it overflow
    float viewW = std::max(0.0f, rect.w - 2.0f * inset_);
    float viewH = std::max(0.0f, rect.h - 2.0f * inset_);
    bool scrollY = wanted.y > viewH;
    bool scrollX = wanted.x > viewW - (scrollY ? bar : 0.0f);
    scrollY = wanted.y > viewH - (scrollX ? bar : 0.0f);
    if (scrollY)
        viewW = std::max(0.0f, viewW - bar);
    if (scrollX)
        viewH = std::max(0.0f, viewH - bar);

    view_[0] = viewW;
    view_[1] = viewH;
    contentSize_[0] = std::max(viewW, wanted.x);
    contentSize_[1] = std::max(viewH, wanted.y);
    for (int axis = 0; axis < 2; ++axis) {
        maxOffset_[axis] = contentSize_[axis] - view_[axis];
        target_[axis] = std::clamp(target_[axis], 0.0f, maxOffset_[axis]);
        offset_[axis] = std::clamp(offset_[axis], 0.0f, maxOffset_[axis]);
    }

    content_->arrange({rect.x + inset_ - std::round(offset_[0]), rect.y + inset_ - std::round(offset_[1]), contentSize_[0],
                       contentSize_[1]},
                      theme);
}

Rect ScrollView::track(int axis) const
{
    if (axis == 0)
        return {rect_.x + inset_, rect_.y + rect_.h - inset_ - kBarWidth, view_[0], kBarWidth};
    return {rect_.x + rect_.w - inset_ - kBarWidth, rect_.y + inset_, kBarWidth, view_[1]};
}

Rect ScrollView::thumb(int axis) const
{
    const Rect t = track(axis);
    const float length = axis == 0 ? t.w : t.h;
    const float size = std::max(kBarWidth * 4.0f, length * view_[axis] / std::max(1.0f, contentSize_[axis]));
    const float travel = std::max(0.0f, length - size);
    const float at = maxOffset_[axis] > 0.0f ? travel * offset_[axis] / maxOffset_[axis] : 0.0f;
    if (axis == 0)
        return {t.x + at, t.y, size, t.h};
    return {t.x, t.y + at, t.w, size};
}

void ScrollView::handleInput(const Input &input)
{
    for (int axis = 0; axis < 2; ++axis) {
        offset_[axis] += (target_[axis] - offset_[axis]) * input.ease(20.0f);
        if (std::abs(target_[axis] - offset_[axis]) < 0.25f)
            offset_[axis] = target_[axis];
    }

    // Content outside the view is hidden, so it mustn't be hovered or clicked there. Drags that
    // started inside keep the real mouse position.
    const bool inside = rect_.contains(input.mouse);
    Input forwarded = input;
    if (!inside || dragging_ >= 0) {
        forwarded.pressed = false;
        if (!input.down)
            forwarded.mouse = {-1e9f, -1e9f};
    }
    content_->handleInput(forwarded);

    // The wheel, unless a scroll view inside this one already used it. Shift turns a vertical
    // wheel sideways.
    if (inside) {
        float across = input.scrollX;
        float down = input.scroll;
        if (input.shift && across == 0.0f)
            std::swap(across, down);
        if (down != 0.0f && maxOffset_[1] > 0.0f) {
            target_[1] = std::clamp(target_[1] - down * kWheelStep, 0.0f, maxOffset_[1]);
            input.scroll = 0.0f;
        }
        if (across != 0.0f && maxOffset_[0] > 0.0f) {
            target_[0] = std::clamp(target_[0] - across * kWheelStep, 0.0f, maxOffset_[0]);
            input.scrollX = 0.0f;
        }
    }

    // The scrollbars: drag a thumb, or click a track to jump there
    if (!input.down)
        dragging_ = -1;
    for (int axis = 0; axis < 2; ++axis) {
        if (maxOffset_[axis] <= 0.0f) {
            barHovered_[axis] = false;
            continue;
        }
        const Rect t = track(axis);
        // A little thicker than it looks, so it's easy to grab
        const Rect grab = axis == 0 ? Rect{t.x, t.y - 6.0f, t.w, t.h + 12.0f} : Rect{t.x - 6.0f, t.y, t.w + 12.0f, t.h};
        barHovered_[axis] = grab.contains(input.mouse);
        const float mouse = along(input.mouse, axis);
        if (input.pressed && barHovered_[axis]) {
            const Rect th = thumb(axis);
            const float start = axis == 0 ? th.x : th.y;
            const float size = axis == 0 ? th.w : th.h;
            if (mouse < start || mouse > start + size) {
                const float origin = axis == 0 ? t.x : t.y;
                const float travel = std::max(1.0f, (axis == 0 ? t.w : t.h) - size);
                target_[axis] = offset_[axis] =
                    std::clamp((mouse - origin - size * 0.5f) / travel * maxOffset_[axis], 0.0f, maxOffset_[axis]);
            }
            dragging_ = axis;
            dragStart_ = mouse;
            dragStartOffset_ = offset_[axis];
        }
        if (dragging_ == axis) {
            const Rect th = thumb(axis);
            const float travel = std::max(1.0f, (axis == 0 ? t.w - th.w : t.h - th.h));
            target_[axis] = offset_[axis] =
                std::clamp(dragStartOffset_ + (mouse - dragStart_) * maxOffset_[axis] / travel, 0.0f, maxOffset_[axis]);
        }
    }
}

void ScrollView::draw(DrawList &list, const Theme &theme) const
{
    // Clip to the view, leaving out the strips the scrollbars sit in
    const float bar = kBarWidth + inset_;
    list.pushClip({rect_.x, rect_.y, rect_.w - (maxOffset_[1] > 0.0f ? bar : 0.0f),
                   rect_.h - (maxOffset_[0] > 0.0f ? bar : 0.0f)});
    content_->draw(list, theme);
    list.popClip();

    for (int axis = 0; axis < 2; ++axis) {
        if (maxOffset_[axis] <= 0.0f)
            continue;
        const bool active = dragging_ == axis || barHovered_[axis];
        list.add(theme.surface(track(axis), Shape::Pill, Elevation::Inset, 0.3f));
        list.add(theme.surface(thumb(axis), Shape::Pill, Elevation::Raised, active ? 0.5f : 0.35f, Finish::Convex));
    }
}

// TitleBar

TitleBar::TitleBar(engine::Window &window)
    : Container(Direction::Row), window_(window), buttons_(std::make_unique<Container>(Direction::Row))
{
    align_ = Align::Center;
    alignSelf_ = Align::Stretch;
    buttons_->setAlign(Align::Center);

    // Flat until hovered, so the bar reads as part of the page
    auto windowButton = [this](Icon icon, std::function<void()> onClick) -> Button & {
        auto &button = buttons_->add<Button>(std::move(onClick));
        button.setIcon(icon).setVariant(ButtonVariant::Flat).setShape(Shape::Circle).setSize(Size::Small);
        return button;
    };
    windowButton(Icon::Minus, [this] { window_.minimize(); });
    maximize_ = &windowButton(Icon::Maximize, [this] { window_.toggleMaximize(); });
    windowButton(Icon::Close, [this] { window_.requestClose(); });
}

TitleBar &TitleBar::setRaised(bool raised, float inset)
{
    raised_ = raised;
    inset_ = std::max(0.0f, inset);
    return *this;
}

bool TitleBar::isDragArea(Vec2 p) const
{
    if (p.y >= rect_.y + rect_.h)
        return false;
    if (showButtons() && buttons_->rect().contains(p))
        return false;
    for (const auto &child : children_) {
        const Component &c = *child;
        if (c.rect().contains(p) && !dynamic_cast<const Label *>(&c) && !dynamic_cast<const Spacer *>(&c))
            return false;
    }
    return true;
}

Vec2 TitleBar::measure(const Theme &theme) const
{
    Vec2 size = Container::measure(theme);
    if (showButtons()) {
        const Vec2 buttons = buttons_->preferredSize(theme);
        size = {size.x + gap(theme) + buttons.x, std::max(size.y, buttons.y)};
    }
    if (raised_)
        size = {size.x + 2.0f * (inset_ + kPadX), size.y + 2.0f * kPadY};
    return size;
}

void TitleBar::arrange(const Rect &rect, const Theme &theme)
{
    bar_ = {rect.x + inset_, rect.y, std::max(0.0f, rect.w - 2.0f * inset_), rect.h};
    const Rect inner = raised_ ? Rect{bar_.x + kPadX, bar_.y + kPadY, std::max(0.0f, bar_.w - 2.0f * kPadX),
                                      std::max(0.0f, bar_.h - 2.0f * kPadY)}
                               : rect;
    Rect rest = inner;
    if (showButtons()) {
        const float width = buttons_->preferredSize(theme).x;
        buttons_->arrange({inner.x + inner.w - width, inner.y, width, inner.h}, theme);
        rest.w = std::max(0.0f, inner.w - width - gap(theme));
    }
    Container::arrange(rest, theme);
    rect_ = rect;
}

void TitleBar::handleInput(const Input &input)
{
    Container::handleInput(input);
    if (showButtons()) {
        maximize_->setIcon(window_.isMaximized() ? Icon::Restore : Icon::Maximize);
        buttons_->handleInput(input);
    }
}

void TitleBar::draw(DrawList &list, const Theme &theme) const
{
    if (raised_)
        list.add(theme.surface(bar_, Shape::Rounded, Elevation::Raised, 0.9f, Finish::Convex));
    Container::draw(list, theme);
    if (showButtons())
        buttons_->draw(list, theme);
}

// DrawerLayout

DrawerLayout::DrawerLayout()
    : content_(std::make_unique<Container>(Direction::Column)),
      drawer_(std::make_unique<Panel>(Direction::Column, Elevation::Raised))
{
    content_->setAlign(Align::Stretch);
    drawer_->setAlign(Align::Stretch);
    grow_ = 1.0f;
    alignSelf_ = Align::Stretch;
}

DrawerLayout &DrawerLayout::setDrawerWidth(float width)
{
    width_ = std::max(160.0f, width);
    return *this;
}

Vec2 DrawerLayout::measure(const Theme &theme) const
{
    return content_->preferredSize(theme);
}

void DrawerLayout::arrange(const Rect &rect, const Theme &theme)
{
    rect_ = rect;
    content_->arrange(rect, theme);

    // Fully closed, the drawer and its shadow sit just past the left edge of the window
    const float hidden = width_ + rect.x + 2.0f * theme.shadowExtent() + 8.0f;
    const float eased = 1.0f - (1.0f - progress_) * (1.0f - progress_);
    drawer_->arrange({rect.x - hidden * (1.0f - eased), rect.y, width_, rect.h}, theme);
}

void DrawerLayout::handleInput(const Input &input)
{
    if (hooks_.attach(input.signals)) {
        hooks_.key(Priority::Component, [this](const KeyEvent &e) {
            if (!open_ || !e.down || e.key != Key::Escape)
                return false;
            close();
            return true;
        });
    }
    progress_ += ((open_ ? 1.0f : 0.0f) - progress_) * input.ease(12.0f);
    if (std::abs(progress_ - (open_ ? 1.0f : 0.0f)) < 0.001f)
        progress_ = open_ ? 1.0f : 0.0f;

    if (!open_ && progress_ <= 0.0f) {
        content_->handleInput(input);
        return;
    }

    // Content keeps animating but can't be hovered, clicked or typed into (no signals: its
    // hooks aren't live)
    Input blocked;
    blocked.mouse = {-1e9f, -1e9f};
    blocked.released = input.released;
    blocked.time = input.time;
    blocked.dt = input.dt;
    content_->handleInput(blocked);

    if (open_) {
        drawer_->handleInput(input);
        const bool outside = rect_.contains(input.mouse) && !drawer_->rect().contains(input.mouse);
        if (input.pressed && outside)
            close();
    } else {
        drawer_->handleInput(blocked);
    }
}

void DrawerLayout::draw(DrawList &list, const Theme &theme) const
{
    content_->draw(list, theme);
    if (progress_ <= 0.0f)
        return;
    // The veil also covers the window padding to the sides and below, but not what's above
    const float pad = theme.padding();
    list.add(theme.scrim({rect_.x - pad, rect_.y, rect_.w + 2.0f * pad, rect_.h + pad}, 0.6f * progress_));
    drawer_->draw(list, theme);
}

}
