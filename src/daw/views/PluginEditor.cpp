#include "daw/views/PluginEditor.h"

#include <cmath>
#include <cstdio>

namespace daw {

using namespace ui;

namespace {

constexpr float kInset = 6.0f; // the well's edge around the picture

}

uint32_t virtualKey(engine::Key key)
{
    using engine::Key;
    const auto k = static_cast<int>(key);
    if (key >= Key::A && key <= Key::Z)
        return 'A' + static_cast<uint32_t>(k - static_cast<int>(Key::A));
    if (key >= Key::Num0 && key <= Key::Num9)
        return '0' + static_cast<uint32_t>(k - static_cast<int>(Key::Num0));
    if (key >= Key::F1 && key <= Key::F12)
        return 0x70 + static_cast<uint32_t>(k - static_cast<int>(Key::F1));
    if (key >= Key::Keypad0 && key <= Key::Keypad9)
        return 0x60 + static_cast<uint32_t>(k - static_cast<int>(Key::Keypad0));
    switch (key) {
    case Key::Space: return 0x20;
    case Key::Enter: return 0x0D;
    case Key::Escape: return 0x1B;
    case Key::Tab: return 0x09;
    case Key::Backspace: return 0x08;
    case Key::Delete: return 0x2E;
    case Key::Insert: return 0x2D;
    case Key::Left: return 0x25;
    case Key::Up: return 0x26;
    case Key::Right: return 0x27;
    case Key::Down: return 0x28;
    case Key::Home: return 0x24;
    case Key::End: return 0x23;
    case Key::PageUp: return 0x21;
    case Key::PageDown: return 0x22;
    case Key::Minus: return 0xBD;
    case Key::Equal: return 0xBB;
    case Key::LeftBracket: return 0xDB;
    case Key::RightBracket: return 0xDD;
    case Key::Backslash: return 0xDC;
    case Key::Semicolon: return 0xBA;
    case Key::Apostrophe: return 0xDE;
    case Key::Comma: return 0xBC;
    case Key::Period: return 0xBE;
    case Key::Slash: return 0xBF;
    case Key::Grave: return 0xC0;
    case Key::KeypadDecimal: return 0x6E;
    case Key::KeypadDivide: return 0x6F;
    case Key::KeypadMultiply: return 0x6A;
    case Key::KeypadSubtract: return 0x6D;
    case Key::KeypadAdd: return 0x6B;
    case Key::Shift: return 0x10;
    case Key::Ctrl: return 0x11;
    case Key::Alt: return 0x12;
    default: return 0;
    }
}

PluginEditorView::PluginEditorView(std::function<plugin::Slot *()> slot, std::string name)
    : slot_(std::move(slot)), name_(std::move(name)), status_("Opening " + name_ + "...")
{
}

Vec2 PluginEditorView::measure(const Theme &) const
{
    if (image_.width == 0)
        return {420.0f, 240.0f};
    return {static_cast<float>(image_.width) + 2.0f * kInset, static_cast<float>(image_.height) + 2.0f * kInset};
}

Rect PluginEditorView::pictureRect() const
{
    // Whole pixels, so the picture lands pixel for pixel
    return {std::round(rect_.x + kInset), std::round(rect_.y + kInset), static_cast<float>(image_.width), static_cast<float>(image_.height)};
}

bool PluginEditorView::focused() const { return hooks_.signals() && hooks_.signals()->focus() == this; }

void PluginEditorView::send(plugin::GuiInputKind kind, int32_t x, int32_t y, uint32_t button, int32_t wheel, uint32_t key)
{
    plugin::Slot *s = slot_();
    if (!s)
        return;
    plugin::GuiInput in;
    in.kind = kind;
    in.x = x;
    in.y = y;
    in.button = button;
    in.wheel = wheel;
    in.key = key;
    in.mods = (mods_ & engine::ModShift ? 1u : 0u) | (mods_ & engine::ModCtrl ? 2u : 0u) | (mods_ & engine::ModAlt ? 4u : 0u);
    s->sendEditorInput(in);
}

void PluginEditorView::makeHooks()
{
    // While it has the focus, keys go to the plugin; Escape gives the focus back
    hooks_.key(Priority::Focus, [this](const engine::KeyEvent &e) {
        if (!focused())
            return false;
        if (e.key == engine::Key::Escape && e.down) {
            hooks_.signals()->setFocus(nullptr);
            return true;
        }
        if (const uint32_t vk = virtualKey(e.key))
            send(e.down ? plugin::GuiInputKind::KeyDown : plugin::GuiInputKind::KeyUp, lastX_, lastY_, 0, 0, vk);
        return true;
    });
    hooks_.text(Priority::Focus, [this](const engine::TextEvent &e) {
        if (!focused())
            return false;
        if (e.codepoint < 0x10000)
            send(plugin::GuiInputKind::Char, lastX_, lastY_, 0, 0, e.codepoint);
        return true;
    });
}

void PluginEditorView::handleInput(const Input &input)
{
    if (hooks_.attach(input.signals))
        makeHooks();
    mods_ = input.mods;
    plugin::Slot *s = slot_();
    using State = plugin::Slot::State;
    if (!s) {
        status_ = name_ + " is no longer here";
        image_.width = image_.height = 0;
        return;
    }
    if (s->state() == State::Loading) {
        status_ = "Loading " + name_ + "...";
    } else if (s->state() != State::Ready) {
        status_ = s->message();
        image_.width = image_.height = 0;
        return;
    } else if (!s->canEmbedEditor()) {
        status_ = name_ + "'s interface only opens as a window of its own";
        return;
    } else if (!s->embeddedEditorOpen() && !s->editorOpening() && input.time - lastAsk_ > 2.0) {
        // Opened (again, after a reload) in the background
        lastAsk_ = input.time;
        s->openEmbeddedEditor();
        if (!s->editorError().empty())
            status_ = s->editorError();
    }

    if (!s->embeddedEditorOpen()) {
        placed_ = false; // opened again: told again
        clip_[0] = clip_[1] = clip_[2] = clip_[3] = -1;
    }
    lastSeen_ = input.time;

    // The newest picture
    uint32_t w = 0, h = 0;
    if (s->readEditorFrame(image_.pixels, w, h, sequence_)) {
        image_.width = w;
        image_.height = h;
        ++image_.version;
    }
    if (image_.width == 0)
        return;

    const Rect area = pictureRect();
    // Where it is on the desktop, for the plugin's own (invisible) window to sit over it, taking
    // the real mouse and keys; told again a few times a second, so it stays just above this
    // window as windows are raised. While one of the app's menus or dialogs covers it, the
    // plugin's window stands aside so the menu gets the mouse.
    if (Context *context = Context::active()) {
        const Vec2 at = context->toScreen({area.x, area.y});
        const int32_t sx = static_cast<int32_t>(std::lround(at.x)), sy = static_cast<int32_t>(std::lround(at.y));
        void *window = context->window().nativeHandle();
        const bool covered = context->overlayCovers(area);
        // Only the part of the picture this view shows may take the mouse (a docked card smaller
        // than the plugin cuts the rest off)
        const float x0 = std::max(area.x, rect_.x), y0 = std::max(area.y, rect_.y);
        const float x1 = std::min(area.x + area.w, rect_.x + rect_.w), y1 = std::min(area.y + area.h, rect_.y + rect_.h);
        int32_t clip[4] = {static_cast<int32_t>(std::lround(x0 - area.x)), static_cast<int32_t>(std::lround(y0 - area.y)),
                           static_cast<int32_t>(std::lround(std::max(0.0f, x1 - x0))), static_cast<int32_t>(std::lround(std::max(0.0f, y1 - y0)))};
        if (clip[0] == 0 && clip[1] == 0 && clip[2] == static_cast<int32_t>(image_.width) && clip[3] == static_cast<int32_t>(image_.height))
            clip[2] = clip[3] = 0; // all of it
        if (!std::equal(clip, clip + 4, clip_)) {
            s->clipEditor(clip[0], clip[1], clip[2] == 0 && clip[3] == 0 ? 0 : std::max(1, clip[2]), clip[2] == 0 && clip[3] == 0 ? 0 : std::max(1, clip[3]));
            std::copy(clip, clip + 4, clip_);
        }
        if (covered) {
            if (placed_) {
                s->placeEditor(0, 0, false);
                placed_ = false;
            }
        } else if (!placed_ || sx != placedX_ || sy != placedY_ || window != placedWindow_ || input.time - placedAt_ > 0.25) {
            // The mouse over it (the plugin's window gets it, so read on the desktop): a press there
            // may bring the plugin's window to the front, which only a program in front may allow
            int mx = 0, my = 0;
            if (placed_ && input.time - allowedAt_ > 0.5 && engine::Window::desktopCursor(mx, my) && mx >= sx && my >= sy &&
                mx < sx + static_cast<int32_t>(image_.width) && my < sy + static_cast<int32_t>(image_.height)) {
                s->allowEditorToFront();
                allowedAt_ = input.time;
            }
            s->placeEditor(sx, sy, true, window);
            placed_ = true;
            placedX_ = sx;
            placedY_ = sy;
            placedWindow_ = window;
            placedAt_ = input.time;
        }
    }
    const bool over = area.contains(input.mouse);
    const int32_t x = static_cast<int32_t>(std::floor(input.mouse.x - area.x));
    const int32_t y = static_cast<int32_t>(std::floor(input.mouse.y - area.y));
    if ((over || dragging_) && (x != lastX_ || y != lastY_)) {
        send(plugin::GuiInputKind::Move, x, y);
        lastX_ = x;
        lastY_ = y;
    }
    if (inside_ && !over && !dragging_)
        send(plugin::GuiInputKind::Leave, x, y);
    inside_ = over;

    if (input.pressed) {
        if (over) {
            const bool twice = input.time - lastPress_ < 0.35;
            lastPress_ = input.time;
            send(twice ? plugin::GuiInputKind::DoubleClick : plugin::GuiInputKind::Down, x, y, 0);
            dragging_ = true;
            if (hooks_.signals())
                hooks_.signals()->setFocus(this);
        } else if (focused()) {
            hooks_.signals()->setFocus(nullptr);
        }
    }
    if (input.released && dragging_) {
        send(plugin::GuiInputKind::Up, x, y, 0);
        dragging_ = false;
    }
    if (over && input.secondaryPressed) {
        input.secondaryPressed = false; // the plugin's own menu, not the app's
        send(plugin::GuiInputKind::Down, x, y, 1);
        send(plugin::GuiInputKind::Up, x, y, 1);
    }
    if (over && input.middlePressed) {
        input.middlePressed = false;
        middleHeld_ = true;
        send(plugin::GuiInputKind::Down, x, y, 2);
    }
    if (middleHeld_ && !input.middleDown) {
        middleHeld_ = false;
        send(plugin::GuiInputKind::Up, x, y, 2);
    }
    if (over && input.scroll != 0.0f) {
        send(plugin::GuiInputKind::Wheel, x, y, 0, static_cast<int32_t>(std::lround(input.scroll * 120.0f)));
        input.scroll = 0.0f;
    }
}

void PluginEditorView::hideIfUnseen(double now)
{
    if (!placed_ || now - lastSeen_ < 0.15)
        return;
    placed_ = false;
    if (plugin::Slot *s = slot_())
        s->placeEditor(0, 0, false);
}

void PluginEditorView::draw(DrawList &list, const Theme &theme) const
{
    list.add(theme.surface(rect_, Shape::Soft, Elevation::Inset, 0.7f));
    if (image_.width == 0) {
        list.add(theme.text(status_, rect_, TextStyle::Body, TextTone::Muted, TextAlign::Center));
        return;
    }
    list.pushClip(rect_);
    list.add(ImageDraw{pictureRect(), &image_, 1.0f});
    list.popClip();
    // A fine accent line under it while it has the keys
    if (focused()) {
        const Rect r = pictureRect();
        list.add(theme.accent({r.x, r.y + r.h + 2.0f, r.w, 2.0f}, Shape::Pill));
    }
}

}
