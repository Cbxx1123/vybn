#include "Dialogs.h"

#include <algorithm>
#include <utility>

namespace ui {

namespace {

constexpr float kMinWidth = 220.0f;
constexpr float kMinHeight = 140.0f;
constexpr float kGrip = 18.0f;        // resize handle in the bottom-right corner
constexpr float kKeepVisible = 80.0f; // pixels of header that always stay in the window

// A small round window button: level until hovered, pressed in while held
void windowButton(DrawList &list, const Theme &theme, const Rect &r, const ClickState &click, Icon icon)
{
    if (click.held())
        list.add(theme.surface(r, Shape::Circle, Elevation::Inset, 0.4f));
    else if (click.hovered)
        list.add(theme.surface(r, Shape::Circle, Elevation::Raised, 0.4f));
    drawIcon(list, theme, icon, r.inset(r.w * 0.22f), click.hovered ? TextTone::Accent : TextTone::Secondary);
}

}

// Dialog

Dialog::Dialog(Context &context, std::string title, std::string message)
    : context_(context), panel_(std::make_unique<Panel>(Direction::Column))
{
    panel_->setAlign(Align::Stretch);
    panel_->setDepth(Depth::Deep);

    auto &header = panel_->add<Container>(Direction::Row);
    header.setAlign(Align::Center);
    header.add<Label>(std::move(title), TextStyle::Title);
    header.add<Spacer>();
    header.add<Button>([this] { close(); })
        .setIcon(Icon::Close)
        .setVariant(ButtonVariant::Flat)
        .setShape(Shape::Circle)
        .setSize(Size::Small);

    message_ = &panel_->add<Label>(std::move(message), TextStyle::Body, TextTone::Secondary);
    body_ = &panel_->add<Container>(Direction::Column);
    body_->setAlign(Align::Stretch);
    buttons_ = &panel_->add<Container>(Direction::Row);
    buttons_->setAlign(Align::Center).setJustify(Justify::End);

    // Modal: while open, keys and actions stop here, so nothing under the dialog reacts to them.
    // Escape closes it. A focused text field inside it is ahead of this (Priority::Focus).
    // Shortcuts (Ctrl, Alt, Super chords) go on to the keymap, so a focused field gets Ctrl+A/C/X/V;
    // the action hook below still stops any app command from running underneath.
    keys_ = context.signals().key.connect([this](const KeyEvent &e) {
        if (e.down && e.key == Key::Escape && e.mods == 0)
            close();
        return (e.mods & (engine::ModCtrl | engine::ModAlt | engine::ModSuper)) == 0;
    }, Priority::Modal);
    actions_ = context.signals().action.connect([](const ActionEvent &) { return true; }, Priority::Modal);
}

Dialog &Dialog::open(Context &context, std::string title, std::string message)
{
    OverlayOptions options;
    options.modal = true;
    options.closeOnOutsideClick = false;
    return context.openOverlay<Dialog>(std::move(options), context, std::move(title), std::move(message));
}

Dialog &Dialog::addButton(std::string label, std::function<void()> action, bool primary)
{
    auto &button = buttons_->add<Button>([this, action = std::move(action)] {
        close();
        if (action)
            action();
    });
    button.setLabel(std::move(label));
    if (primary)
        button.setVariant(ButtonVariant::Accent).setShape(Shape::Pill);
    else
        button.setVariant(ButtonVariant::Flat);
    return *this;
}

Dialog &Dialog::setWidth(float width)
{
    width_ = std::max(280.0f, width);
    return *this;
}

Dialog &Dialog::onClose(std::function<void()> handler)
{
    onClose_ = std::move(handler);
    return *this;
}

void Dialog::close()
{
    if (onClose_) {
        auto handler = std::move(onClose_);
        onClose_ = nullptr;
        handler();
    }
    context_.closeOverlay(*this);
}

Vec2 Dialog::measure(const Theme &theme) const
{
    const Vec2 size = panel_->preferredSize(theme);
    return {std::max(size.x, width_), size.y};
}

void Dialog::arrange(const Rect &window, const Theme &theme)
{
    // The message wraps to the dialog's width
    message_->setWrapWidth(width_ - 2.0f * theme.padding());
    const Vec2 size = measure(theme);
    // Rises slightly into place as it appears
    const float lift = (1.0f - shown_) * 16.0f;
    rect_ = {window.x + (window.w - size.x) * 0.5f, window.y + (window.h - size.y) * 0.45f + lift, size.x, size.y};
    panel_->arrange(rect_, theme);
}

void Dialog::handleInput(const Input &input)
{
    shown_ += (1.0f - shown_) * input.ease(16.0f);
    panel_->handleInput(input);
}

void Dialog::draw(DrawList &list, const Theme &theme) const
{
    panel_->draw(list, theme);
}

// FloatingWindow

FloatingWindow::FloatingWindow(Context &context, std::string title, Rect frame)
    : context_(context), title_(std::move(title)), frame_(frame), scroll_(std::make_unique<ScrollView>())
{
    frame_.w = std::max(frame_.w, kMinWidth);
    frame_.h = std::max(frame_.h, kMinHeight);
}

FloatingWindow &FloatingWindow::open(Context &context, std::string title, Rect frame, std::function<void()> onClose)
{
    OverlayOptions options;
    options.closeOnOutsideClick = false;
    options.onClose = std::move(onClose);
    return context.openOverlay<FloatingWindow>(std::move(options), context, std::move(title), frame);
}

FloatingWindow &FloatingWindow::onPopOut(std::function<void(FloatingWindow &)> handler)
{
    popOut_ = std::move(handler);
    return *this;
}

void FloatingWindow::close()
{
    context_.closeOverlay(*this);
}

Rect FloatingWindow::grip() const
{
    return {frame_.x + frame_.w - kGrip, frame_.y + frame_.h - kGrip, kGrip, kGrip};
}

void FloatingWindow::arrange(const Rect &window, const Theme &theme)
{
    // Keep enough of the header on screen to grab it again
    frame_.x = std::clamp(frame_.x, window.x - frame_.w + kKeepVisible, window.x + window.w - kKeepVisible);
    frame_.y = std::clamp(frame_.y, window.y, std::max(window.y, window.y + window.h - kKeepVisible * 0.5f));
    rect_ = frame_;

    const float headerHeight = theme.controlHeight(Size::Small) + 12.0f;
    header_ = {frame_.x, frame_.y, frame_.w, headerHeight};
    const float button = theme.controlHeight(Size::Small) * 0.8f;
    const float y = frame_.y + (headerHeight - button) * 0.5f;
    closeRect_ = {frame_.x + frame_.w - 10.0f - button, y, button, button};
    popOutRect_ = popOut_ ? Rect{closeRect_.x - 4.0f - button, y, button, button} : Rect{};

    scroll_->arrange({frame_.x + 4.0f, frame_.y + headerHeight, frame_.w - 8.0f, frame_.h - headerHeight - 4.0f}, theme);
}

void FloatingWindow::handleInput(const Input &input)
{
    if (closeClick_.update(closeRect_, input, true)) {
        close();
        return;
    }
    if (popOut_ && popOutClick_.update(popOutRect_, input, true)) {
        popOut_(*this);
        return;
    }

    if (input.pressed && frame_.contains(input.mouse)) {
        context_.bringToFront(*this);
        const bool onButton = closeRect_.contains(input.mouse) || (popOut_ && popOutRect_.contains(input.mouse));
        if (grip().contains(input.mouse)) {
            resizing_ = true;
            grab_ = {frame_.x + frame_.w - input.mouse.x, frame_.y + frame_.h - input.mouse.y};
        } else if (header_.contains(input.mouse) && !onButton) {
            dragging_ = true;
            grab_ = {input.mouse.x - frame_.x, input.mouse.y - frame_.y};
        }
    }
    if (!input.down)
        dragging_ = resizing_ = false;
    gripHovered_ = grip().contains(input.mouse) || resizing_;

    if (dragging_) {
        frame_.x = input.mouse.x - grab_.x;
        frame_.y = input.mouse.y - grab_.y;
    } else if (resizing_) {
        frame_.w = std::max(kMinWidth, input.mouse.x + grab_.x - frame_.x);
        frame_.h = std::max(kMinHeight, input.mouse.y + grab_.y - frame_.y);
    }

    // The content doesn't react while the window itself is being moved or resized
    scroll_->handleInput(dragging_ || resizing_ ? input.occluded() : input);
}

void FloatingWindow::draw(DrawList &list, const Theme &theme) const
{
    list.add(theme.surface(frame_, Shape::Rounded, Elevation::Raised, theme.depthScale(Depth::Deep)));

    const float pad = 16.0f;
    const float right = (popOut_ ? popOutRect_.x : closeRect_.x) - 8.0f;
    list.add(theme.text(title_, {frame_.x + pad, header_.y, std::max(0.0f, right - frame_.x - pad), header_.h},
                        theme.fontSize(TextStyle::Body), TextAlign::Start,
                        theme.textColor(dragging_ ? TextTone::Accent : TextTone::Primary)));
    if (popOut_)
        windowButton(list, theme, popOutRect_, popOutClick_, Icon::PopOut);
    windowButton(list, theme, closeRect_, closeClick_, Icon::Close);
    // A groove under the header
    list.add(theme.surface({frame_.x + pad, header_.y + header_.h - 2.0f, frame_.w - 2.0f * pad, 3.0f}, Shape::Pill,
                           Elevation::Inset, 0.2f));

    list.pushClip(frame_);
    scroll_->draw(list, theme);
    list.popClip();

    // Resize grip: three dots along the corner's diagonal
    const Rect g = grip();
    const TextTone tone = gripHovered_ ? TextTone::Accent : TextTone::Muted;
    for (int i = 0; i < 3; ++i) {
        const float d = 3.0f;
        const float offset = 4.0f + i * 4.5f;
        list.add(theme.ink({g.x + g.w - offset - d, g.y + g.h - 4.0f - d - (2 - i) * 4.5f, d, d}, Shape::Circle, tone));
    }
}

}
