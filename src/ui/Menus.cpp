#include "Menus.h"

#include <algorithm>
#include <utility>

namespace ui {

namespace {

constexpr float kMenuPadding = 6.0f;    // inside the popup, around the rows
constexpr float kSeparatorHeight = 11.0f;
constexpr float kCheckColumn = 30.0f;   // left of the labels, for check marks
constexpr float kScreenMargin = 4.0f;   // popups keep this far from the window edges
constexpr float kRevealSlide = 14.0f;   // how far the rows drop into place as a menu opens

}

// MenuItem

MenuItem MenuItem::item(std::string label, std::function<void()> action, std::string actionId)
{
    MenuItem m;
    m.label = std::move(label);
    m.action = std::move(action);
    m.actionId = std::move(actionId);
    return m;
}

MenuItem MenuItem::toggle(std::string label, std::function<bool()> checked, std::function<void()> action,
                          std::string actionId)
{
    MenuItem m = item(std::move(label), std::move(action), std::move(actionId));
    m.checked = std::move(checked);
    return m;
}

MenuItem MenuItem::sub(std::string label, std::vector<MenuItem> items)
{
    MenuItem m;
    m.label = std::move(label);
    m.submenu = std::move(items);
    return m;
}

MenuItem MenuItem::divider()
{
    MenuItem m;
    m.separator = true;
    return m;
}

MenuItem MenuItem::when(std::function<bool()> condition) const
{
    MenuItem m = *this;
    m.enabled = std::move(condition);
    return m;
}

MenuItem MenuItem::labelled(std::function<std::string()> labelOf) const
{
    MenuItem m = *this;
    m.dynamicLabel = std::move(labelOf);
    return m;
}

// MenuPopup

MenuPopup::MenuPopup(Context &context, std::vector<MenuItem> items, Vec2 anchor, float minWidth)
    : context_(context), items_(std::move(items)), anchor_(anchor), minWidth_(minWidth)
{
    for (MenuItem &item : items_) {
        if (item.dynamicLabel)
            item.label = item.dynamicLabel();
    }
}

float MenuPopup::rowHeight(const Theme &theme, const MenuItem &item) const
{
    return item.separator ? kSeparatorHeight : std::round(theme.controlHeight(Size::Small) * 0.9f);
}

Vec2 MenuPopup::measure(const Theme &theme) const
{
    float width = minWidth_;
    float height = 2.0f * kMenuPadding;
    for (const MenuItem &item : items_) {
        height += rowHeight(theme, item);
        if (item.separator)
            continue;
        float w = kCheckColumn + theme.measureText(item.label, Size::Small).x + 20.0f;
        const std::string keys = context_.keymap().shortcutText(item.actionId);
        if (!keys.empty())
            w += 36.0f + theme.measureText(keys, TextStyle::Caption).x;
        if (!item.submenu.empty())
            w += 24.0f;
        width = std::max(width, w + 2.0f * kMenuPadding);
    }
    return {std::max(width, 160.0f), height};
}

void MenuPopup::arrange(const Rect &window, const Theme &theme)
{
    const Vec2 size = measure(theme);
    // Keep the whole menu in the window: slide it back from the right and bottom edges
    float x = std::min(anchor_.x, window.x + window.w - kScreenMargin - size.x);
    float y = std::min(anchor_.y, window.y + window.h - kScreenMargin - size.y);
    rect_ = {std::max(window.x + kScreenMargin, x), std::max(window.y + kScreenMargin, y), size.x, size.y};

    rows_.clear();
    float rowY = rect_.y + kMenuPadding;
    for (const MenuItem &item : items_) {
        const float h = rowHeight(theme, item);
        rows_.push_back({rect_.x + kMenuPadding, rowY, rect_.w - 2.0f * kMenuPadding, h});
        rowY += h;
    }

    if (submenu_) {
        // Beside its item, overlapping slightly; flipped to the left if it doesn't fit
        const Rect &row = rows_[submenuIndex_];
        const float width = submenu_->measure(theme).x;
        float sx = rect_.x + rect_.w - 4.0f;
        if (sx + width > window.x + window.w - kScreenMargin)
            sx = rect_.x - width + 4.0f;
        submenu_->anchor_ = {sx, row.y - kMenuPadding};
        submenu_->arrange(window, theme);
    }
}

bool MenuPopup::hitTest(Vec2 p) const
{
    return rect_.contains(p) || (submenu_ && submenu_->hitTest(p));
}

void MenuPopup::openSubmenu(int index)
{
    if (submenuIndex_ == index && submenu_)
        return;
    submenu_ = std::make_unique<MenuPopup>(context_, items_[index].submenu, Vec2{rect_.x + rect_.w, rows_[index].y});
    submenu_->root_ = &root();
    submenuIndex_ = index;
}

void MenuPopup::activate(int index)
{
    if (index < 0 || index >= static_cast<int>(items_.size()) || !items_[index].isEnabled())
        return;
    const MenuItem &item = items_[index];
    if (!item.submenu.empty()) {
        openSubmenu(index);
        return;
    }
    // Close first: the action may open another menu or dialog
    context_.closeOverlay(root());
    if (item.action)
        item.action();
    else if (!item.actionId.empty())
        context_.signals().trigger(item.actionId);
}

void MenuPopup::move(int step)
{
    const int count = static_cast<int>(items_.size());
    if (count == 0)
        return;
    int index = highlighted_;
    for (int tries = 0; tries < count; ++tries) {
        index = index < 0 ? (step > 0 ? 0 : count - 1) : (index + step + count) % count;
        if (items_[index].isEnabled()) {
            highlighted_ = index;
            return;
        }
    }
}

void MenuPopup::keyPressed(Key key)
{
    switch (key) {
    case Key::Down:
        move(1);
        break;
    case Key::Up:
        move(-1);
        break;
    case Key::Enter:
    case Key::Space:
        activate(highlighted_);
        break;
    case Key::Right:
        if (highlighted_ >= 0 && !items_[highlighted_].submenu.empty()) {
            openSubmenu(highlighted_);
            submenu_->move(1);
        }
        break;
    case Key::Left:
        if (root_)
            closeRequested_ = true;
        break;
    case Key::Escape:
        context_.closeOverlay(root());
        break;
    default:
        break;
    }
}

void MenuPopup::handleInput(const Input &input)
{
    // Unfolds toward fully open, easing out so it lands softly
    if (reveal_ < 1.0f) {
        reveal_ += (1.0f - reveal_) * input.ease(16.0f);
        if (reveal_ > 0.995f)
            reveal_ = 1.0f;
    }

    // The top-level menu takes the keyboard for the whole menu: keys go to the deepest open
    // submenu, and nothing else gets keys or actions while it is open
    if (!root_ && hooks_.attach(input.signals)) {
        hooks_.key(Priority::Popup, [this](const KeyEvent &e) {
            if (e.down) {
                MenuPopup *deepest = this;
                while (deepest->submenu_)
                    deepest = deepest->submenu_.get();
                deepest->keyPressed(e.key);
            }
            return true;
        });
        hooks_.text(Priority::Popup, [](const TextEvent &) { return true; });
        hooks_.anyAction(Priority::Popup, [](const ActionEvent &) { return true; });
    }

    // The submenu is on top: it sees the input first, and has the keyboard while open
    bool overSubmenu = false;
    if (submenu_) {
        submenu_->handleInput(input);
        overSubmenu = submenu_->hitTest(input.mouse);
        if (submenu_->closeRequested_) {
            submenu_.reset();
            submenuIndex_ = -1;
        }
    }

    // The mouse highlights rows when it moves, so it doesn't fight the arrow keys
    const bool mouseMoved = input.mouse.x != lastMouse_.x || input.mouse.y != lastMouse_.y;
    lastMouse_ = input.mouse;
    if (!overSubmenu && (mouseMoved || input.pressed || input.released)) {
        int hovered = -1;
        for (size_t i = 0; i < rows_.size(); ++i) {
            if (rows_[i].contains(input.mouse) && items_[i].isEnabled())
                hovered = static_cast<int>(i);
        }
        if (hovered >= 0 || rect_.contains(input.mouse))
            highlighted_ = hovered;
        // Hovering a submenu item opens it; hovering another item closes the open one
        if (hovered >= 0 && !items_[hovered].submenu.empty())
            openSubmenu(hovered);
        else if (hovered >= 0 && submenu_) {
            submenu_.reset();
            submenuIndex_ = -1;
        }
    }

    // Items act on release, so press-drag-release across a menu works as it does natively
    if (!overSubmenu) {
        if (input.released && highlighted_ >= 0 && rows_[highlighted_].contains(input.mouse))
            activate(highlighted_);
    }

}

void MenuPopup::draw(DrawList &list, const Theme &theme) const
{
    // The menu is revealed top-down, and its rows drop into place inside that reveal
    const float e = reveal_;
    const float m = theme.shadowExtent();
    const float top = rect_.y - m;
    const float bottom = e >= 1.0f ? rect_.y + rect_.h + m : rect_.y + rect_.h * e;
    list.pushClip({rect_.x - m, top, rect_.w + 2.0f * m, std::max(0.0f, bottom - top)});

    list.add(theme.surface(rect_, Shape::Soft, Elevation::Raised, theme.depthScale(Depth::Normal)));

    const float slide = (1.0f - e) * -kRevealSlide;
    const float px = theme.fontSize(Size::Small);
    for (size_t i = 0; i < items_.size(); ++i) {
        const MenuItem &item = items_[i];
        Rect row = rows_[i];
        row.y += slide;
        if (item.separator) {
            list.add(theme.surface({row.x + 8.0f, row.y + row.h * 0.5f - 1.5f, row.w - 16.0f, 3.0f}, Shape::Pill,
                                   Elevation::Inset, 0.2f));
            continue;
        }
        const bool enabled = item.isEnabled();
        const bool lit = static_cast<int>(i) == highlighted_ || static_cast<int>(i) == submenuIndex_;
        if (lit && enabled)
            list.add(theme.surface(row, Shape::Soft, Elevation::Inset, 0.4f));

        const TextTone tone = !enabled ? TextTone::Muted : lit ? TextTone::Accent : TextTone::Primary;
        if (item.checked && item.checked())
            list.add(theme.accent({row.x + kCheckColumn * 0.5f - 4.0f, row.y + row.h * 0.5f - 4.0f, 8.0f, 8.0f}, Shape::Circle,
                                  enabled ? 1.0f : 0.4f));
        list.add(theme.text(item.label, {row.x + kCheckColumn, row.y, row.w - kCheckColumn, row.h}, px, TextAlign::Start,
                            theme.textColor(tone)));

        float right = row.x + row.w - 10.0f;
        if (!item.submenu.empty()) {
            drawIcon(list, theme, Icon::Chevron, {right - 16.0f, row.y + row.h * 0.5f - 8.0f, 16.0f, 16.0f}, tone, enabled);
            right -= 22.0f;
        }
        const std::string keys = context_.keymap().shortcutText(item.actionId);
        if (!keys.empty())
            list.add(theme.text(keys, {row.x, row.y, right - row.x, row.h}, theme.fontSize(TextStyle::Caption),
                                TextAlign::End, theme.textColor(enabled ? TextTone::Secondary : TextTone::Muted)));
    }

    list.popClip();

    if (submenu_)
        submenu_->draw(list, theme);
}

MenuPopup &openContextMenu(Context &context, std::vector<MenuItem> items, Vec2 at)
{
    // In the window being used, should the content have moved to another one
    Context &in = Context::activeOr(context);
    return in.openOverlay<MenuPopup>(OverlayOptions{}, in, std::move(items), at);
}

// MenuBar

MenuBar::MenuBar(Context &context)
    : context_(context)
{
}

MenuBar &MenuBar::addMenu(std::string title, std::vector<MenuItem> items)
{
    // Every item that names an action runs when the action does
    std::function<void(const std::vector<MenuItem> &)> hook = [&](const std::vector<MenuItem> &list) {
        for (const MenuItem &item : list) {
            hook(item.submenu);
            if (item.actionId.empty() || !item.action)
                continue;
            actions_.push_back(context_.signals().onAction(item.actionId, [this, item](const ActionEvent &) {
                if (!enabled_ || !item.isEnabled())
                    return false;
                item.action();
                return true;
            }, Priority::App));
        }
    };
    hook(items);
    menus_.push_back({std::move(title), std::move(items), {}});
    return *this;
}

Vec2 MenuBar::measure(const Theme &theme) const
{
    // The titles sit in one inset track, a little taller than they are
    const float height = theme.controlHeight(Size::Small);
    float width = 8.0f;
    for (const Menu &menu : menus_)
        width += theme.measureText(menu.title, Size::Small).x + height * 0.7f;
    return {width, height};
}

void MenuBar::arrange(const Rect &rect, const Theme &theme)
{
    rect_ = rect;
    const float track = theme.controlHeight(Size::Small);
    const float height = track - 8.0f;
    float x = rect.x + 4.0f;
    for (Menu &menu : menus_) {
        const float w = theme.measureText(menu.title, Size::Small).x + track * 0.7f;
        menu.rect = {x, rect.y + (rect.h - height) * 0.5f, w, height};
        x += w;
    }
}

void MenuBar::openMenu(int index)
{
    if (index < 0 || index >= static_cast<int>(menus_.size()))
        return;
    if (popup_)
        context_.closeOverlay(*popup_);
    const int generation = ++generation_;
    OverlayOptions options;
    options.onClose = [this, generation] {
        if (generation_ == generation) {
            open_ = -1;
            popup_ = nullptr;
        }
    };
    const Rect &title = menus_[index].rect;
    popup_ = &context_.openOverlay<MenuPopup>(std::move(options), context_, menus_[index].items,
                                              Vec2{title.x - 4.0f, title.y + title.h + 14.0f});
    open_ = index;
}

void MenuBar::handleInput(const Input &input)
{
    if (!enabled_)
        return;
    hovered_ = -1;
    for (size_t i = 0; i < menus_.size(); ++i) {
        if (menus_[i].rect.contains(input.mouse))
            hovered_ = static_cast<int>(i);
    }
    // With a menu open, the others open on hover, as in native menu bars
    if (open_ >= 0 && hovered_ >= 0 && hovered_ != open_)
        openMenu(hovered_);
    else if (input.pressed && hovered_ >= 0)
        openMenu(hovered_);
}

void MenuBar::draw(DrawList &list, const Theme &theme) const
{
    if (menus_.empty())
        return;
    // One inset track; the open (or hovered) title lifts out of it
    const float track = theme.controlHeight(Size::Small);
    const Rect &last = menus_.back().rect;
    list.add(theme.surface({rect_.x, rect_.y + (rect_.h - track) * 0.5f, last.x + last.w + 4.0f - rect_.x, track},
                           Shape::Pill, Elevation::Inset, 0.5f));
    for (size_t i = 0; i < menus_.size(); ++i) {
        const Menu &menu = menus_[i];
        const bool open = static_cast<int>(i) == open_;
        if (open)
            list.add(theme.surface(menu.rect, Shape::Pill, Elevation::Raised, 0.45f, Finish::Convex));
        else if (static_cast<int>(i) == hovered_)
            list.add(theme.surface(menu.rect, Shape::Pill, Elevation::Raised, 0.3f));
        list.add(theme.text(menu.title, menu.rect, theme.fontSize(Size::Small), TextAlign::Center,
                            theme.textColor(!enabled_ ? TextTone::Muted
                                            : open ? TextTone::Accent
                                                   : static_cast<int>(i) == hovered_ ? TextTone::Primary
                                                                                     : TextTone::Secondary)));
    }
}

// ContextMenuArea

ContextMenuArea::ContextMenuArea(Context &context, std::function<std::vector<MenuItem>()> items)
    : Container(Direction::Column), context_(context), items_(std::move(items))
{
    align_ = Align::Stretch;
}

void ContextMenuArea::handleInput(const Input &input)
{
    Container::handleInput(input);
    if (input.secondaryPressed && enabled_ && rect_.contains(input.mouse) && items_) {
        input.secondaryPressed = false; // used up, so enclosing areas don't open a menu too
        openContextMenu(context_, items_(), input.mouse);
    }
}

// Dropdown

Dropdown::Dropdown(Context &context, std::vector<std::string> options, int selected, std::function<void(int)> onChange)
    : context_(context), options_(std::move(options)), selected_(selected), onChange_(std::move(onChange))
{
}

Dropdown &Dropdown::onChange(std::function<void(int)> callback)
{
    onChange_ = std::move(callback);
    return *this;
}

Dropdown &Dropdown::setSelected(int index)
{
    selected_ = index;
    return *this;
}

Dropdown &Dropdown::choose(int index)
{
    if (selected_ != index) {
        selected_ = index;
        if (onChange_)
            onChange_(index);
    }
    return *this;
}

Dropdown &Dropdown::setOptions(std::vector<std::string> options)
{
    options_ = std::move(options);
    return *this;
}

Dropdown &Dropdown::setSize(Size size)
{
    size_ = size;
    return *this;
}

Vec2 Dropdown::measure(const Theme &theme) const
{
    const float height = theme.controlHeight(size_);
    float widest = 0.0f;
    for (const std::string &option : options_)
        widest = std::max(widest, theme.measureText(option, size_).x);
    return {widest + height * 1.4f, height};
}

void Dropdown::handleInput(const Input &input)
{
    if (!click_.update(rect_, input, enabled_) || open_)
        return;

    std::vector<MenuItem> items;
    for (size_t i = 0; i < options_.size(); ++i) {
        const int index = static_cast<int>(i);
        items.push_back(MenuItem::toggle(
            options_[i], [this, index] { return selected_ == index; },
            [this, index] {
                if (selected_ != index) {
                    selected_ = index;
                    if (onChange_)
                        onChange_(index);
                }
            }));
    }
    const int generation = ++generation_;
    OverlayOptions options;
    options.onClose = [this, generation] {
        if (generation_ == generation)
            open_ = false;
    };
    Context &in = Context::activeOr(context_);
    in.openOverlay<MenuPopup>(std::move(options), in, std::move(items), Vec2{rect_.x, rect_.y + rect_.h + 6.0f}, rect_.w);
    open_ = true;
}

void Dropdown::draw(DrawList &list, const Theme &theme) const
{
    if (open_ || click_.held())
        list.add(theme.surface(rect_, Shape::Rounded, Elevation::Inset, 0.6f));
    else
        list.add(theme.surface(rect_, Shape::Rounded, Elevation::Raised, enabled_ ? (click_.hovered ? 0.7f : 0.55f) : 0.3f,
                               Finish::Convex));

    const float pad = rect_.h * 0.4f;
    const std::string &label = selected_ >= 0 && selected_ < static_cast<int>(options_.size()) ? options_[selected_] : "";
    list.add(theme.text(label, {rect_.x + pad, rect_.y, rect_.w - 2.0f * pad, rect_.h}, size_, TextAlign::Start, enabled_));
    const float icon = theme.fontSize(size_) * 1.3f;
    drawIcon(list, theme, Icon::ChevronDown, {rect_.x + rect_.w - pad - icon * 0.7f, rect_.y + (rect_.h - icon) * 0.5f, icon, icon},
             open_ ? TextTone::Accent : TextTone::Secondary, enabled_);
}

}
