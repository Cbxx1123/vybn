#include "Panels.h"

#include <algorithm>
#include <utility>

namespace ui {

// SplitView

SplitView::SplitView(Direction direction, float ratio)
    : first_(std::make_unique<Container>(Direction::Column)),
      second_(std::make_unique<Container>(Direction::Column)),
      direction_(direction),
      ratio_(std::clamp(ratio, 0.0f, 1.0f))
{
    first_->setAlign(Align::Stretch);
    second_->setAlign(Align::Stretch);
    alignSelf_ = Align::Stretch;
}

SplitView &SplitView::setRatio(float ratio)
{
    ratio_ = std::clamp(ratio, 0.0f, 1.0f);
    return *this;
}

SplitView &SplitView::setMinimums(float first, float second)
{
    minFirst_ = std::max(0.0f, first);
    minSecond_ = std::max(0.0f, second);
    return *this;
}

Vec2 SplitView::measure(const Theme &theme) const
{
    // Only the minimums: the panes share whatever the parent gives
    const float main = minFirst_ + theme.spacing() + minSecond_;
    const float cross = 160.0f;
    return direction_ == Direction::Row ? Vec2{main, cross} : Vec2{cross, main};
}

void SplitView::arrange(const Rect &rect, const Theme &theme)
{
    rect_ = rect;
    const bool row = direction_ == Direction::Row;
    const float handle = theme.spacing();
    const float length = row ? rect.w : rect.h;
    available_ = std::max(0.0f, length - handle);

    float a = ratio_ * available_;
    if (available_ >= minFirst_ + minSecond_)
        a = std::clamp(a, minFirst_, available_ - minSecond_);
    a = std::round(a);
    const float b = available_ - a;

    if (row) {
        first_->arrange({rect.x, rect.y, a, rect.h}, theme);
        handle_ = {rect.x + a, rect.y, handle, rect.h};
        second_->arrange({rect.x + a + handle, rect.y, b, rect.h}, theme);
    } else {
        first_->arrange({rect.x, rect.y, rect.w, a}, theme);
        handle_ = {rect.x, rect.y + a, rect.w, handle};
        second_->arrange({rect.x, rect.y + a + handle, rect.w, b}, theme);
    }
}

void SplitView::handleInput(const Input &input)
{
    hovered_ = enabled_ && handle_.contains(input.mouse);
    if (input.pressed && hovered_) {
        dragging_ = true;
        dragOffset_ = along(input.mouse) - along({handle_.x, handle_.y});
    }
    if (!input.down)
        dragging_ = false;
    if (dragging_ && available_ > 0.0f) {
        const float start = along({rect_.x, rect_.y});
        setRatio((along(input.mouse) - dragOffset_ - start) / available_);
    }

    // While the handle is dragged the panes only see the release
    const Input &forwarded = dragging_ ? input.occluded() : input;
    first_->handleInput(forwarded);
    second_->handleInput(forwarded);
}

void SplitView::draw(DrawList &list, const Theme &theme) const
{
    first_->draw(list, theme);
    second_->draw(list, theme);

    // The handle: a short groove across the gap, raised into a grip when hovered or dragged
    const bool row = direction_ == Direction::Row;
    const Vec2 c = handle_.center();
    const float length = 44.0f;
    const float thickness = 6.0f;
    const Rect grip = row ? Rect{c.x - thickness * 0.5f, c.y - length * 0.5f, thickness, length}
                          : Rect{c.x - length * 0.5f, c.y - thickness * 0.5f, length, thickness};
    if (dragging_ || hovered_)
        list.add(theme.surface(grip.inset(-1.0f), Shape::Pill, Elevation::Raised, 0.4f, Finish::Convex));
    else
        list.add(theme.surface(grip, Shape::Pill, Elevation::Inset, 0.3f));
    if (dragging_)
        list.add(theme.accent(grip.inset(2.0f), Shape::Pill, 0.8f));
}

// TabView

TabView::TabView()
    : pages_(std::make_unique<Pages>())
{
    alignSelf_ = Align::Stretch;
}

Container &TabView::addTab(std::string title, bool closable)
{
    tabs_.push_back({std::move(title), closable, {}, {}});
    auto &page = pages_->add<Container>(Direction::Column);
    page.setAlign(Align::Stretch);
    pages_->setCurrent(current_);
    return page;
}

TabView &TabView::setCurrent(size_t index)
{
    if (index < tabs_.size()) {
        current_ = index;
        pages_->setCurrent(index);
    }
    return *this;
}

TabView &TabView::onTabClosed(std::function<void(const std::string &)> callback)
{
    onTabClosed_ = std::move(callback);
    return *this;
}

float TabView::tabWidth(const Theme &theme, const Tab &tab) const
{
    const float height = theme.controlHeight(Size::Small);
    return theme.measureText(tab.title, Size::Small).x + height * 0.9f + (tab.closable ? 18.0f : 0.0f);
}

Vec2 TabView::measure(const Theme &theme) const
{
    const float strip = theme.controlHeight(Size::Small) + 8.0f;
    float tabs = 8.0f;
    for (const Tab &tab : tabs_)
        tabs += tabWidth(theme, tab);
    const Vec2 page = pages_->preferredSize(theme);
    const float pad = 2.0f * theme.padding();
    return {std::max(tabs, page.x + pad), strip + theme.spacing() + page.y + pad};
}

void TabView::arrange(const Rect &rect, const Theme &theme)
{
    rect_ = rect;
    const float stripHeight = theme.controlHeight(Size::Small) + 8.0f;
    float x = rect.x + 4.0f;
    for (Tab &tab : tabs_) {
        const float w = tabWidth(theme, tab);
        tab.rect = {x, rect.y + 4.0f, w, stripHeight - 8.0f};
        tab.close = tab.closable ? Rect{x + w - 28.0f, tab.rect.y + (tab.rect.h - 18.0f) * 0.5f, 18.0f, 18.0f} : Rect{};
        x += w;
    }
    strip_ = {rect.x, rect.y, std::min(rect.w, x + 4.0f - rect.x), stripHeight};

    const float top = rect.y + stripHeight + theme.spacing() * 0.6f;
    body_ = {rect.x, top, rect.w, std::max(0.0f, rect.y + rect.h - top)};
    pages_->arrange(body_.inset(theme.padding()), theme);
}

void TabView::removeTab(size_t index)
{
    const std::string title = tabs_[index].title;
    tabs_.erase(tabs_.begin() + static_cast<long>(index));
    pages_->release(pages_->child(index));
    if (current_ >= tabs_.size())
        current_ = tabs_.empty() ? 0 : tabs_.size() - 1;
    else if (current_ > index)
        --current_;
    pages_->setCurrent(current_);
    if (onTabClosed_)
        onTabClosed_(title);
}

void TabView::handleInput(const Input &input)
{
    hovered_ = closeHovered_ = -1;
    if (enabled_) {
        for (size_t i = 0; i < tabs_.size(); ++i) {
            if (tabs_[i].closable && tabs_[i].close.contains(input.mouse))
                closeHovered_ = static_cast<int>(i);
            else if (tabs_[i].rect.contains(input.mouse))
                hovered_ = static_cast<int>(i);
        }
    }
    if (input.pressed) {
        pressed_ = hovered_;
        closePressed_ = closeHovered_;
        // Tabs switch on press, as native tabs do
        if (hovered_ >= 0)
            setCurrent(static_cast<size_t>(hovered_));
    }

    // The page first; tabs are removed after, so no page is mid-input when it goes
    pages_->handleInput(input);

    if (input.released) {
        if (closePressed_ >= 0 && closePressed_ == closeHovered_)
            removeTab(static_cast<size_t>(closePressed_));
        pressed_ = closePressed_ = -1;
    }
}

void TabView::draw(DrawList &list, const Theme &theme) const
{
    if (!tabs_.empty())
        list.add(theme.surface(strip_, Shape::Pill, Elevation::Inset, 0.55f));
    for (size_t i = 0; i < tabs_.size(); ++i) {
        const Tab &tab = tabs_[i];
        const bool selected = i == current_;
        if (selected)
            list.add(theme.surface(tab.rect, Shape::Pill, Elevation::Raised, 0.45f, Finish::Convex));
        const TextTone tone = selected ? TextTone::Accent : static_cast<int>(i) == hovered_ ? TextTone::Primary : TextTone::Secondary;
        const float right = tab.closable ? tab.close.x - 4.0f : tab.rect.x + tab.rect.w;
        const float pad = tab.rect.h * 0.5f;
        list.add(theme.text(tab.title, {tab.rect.x + pad, tab.rect.y, right - tab.rect.x - pad, tab.rect.h},
                            theme.fontSize(Size::Small), TextAlign::Start, theme.textColor(tone)));
        if (tab.closable) {
            if (static_cast<int>(i) == closeHovered_)
                list.add(theme.surface(tab.close, Shape::Circle, Elevation::Inset, 0.3f));
            drawIcon(list, theme, Icon::Close, tab.close.inset(2.0f), static_cast<int>(i) == closeHovered_ ? TextTone::Accent : TextTone::Muted);
        }
    }

    list.add(theme.surface(body_, Shape::Rounded, Elevation::Raised, theme.depthScale(Depth::Normal)));
    list.pushClip(body_);
    pages_->draw(list, theme);
    list.popClip();
    if (tabs_.empty())
        list.add(theme.text("No open tabs", body_, TextStyle::Body, TextTone::Muted, TextAlign::Center));
}

}
