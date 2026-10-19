#include "Context.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <variant>

namespace ui {

namespace {

// The window itself: no surface, but padded so top-level panels' shadows aren't clipped
class Root : public Container {
public:
    Root()
    {
        setAlign(Align::Center);
        setJustify(Justify::Center);
    }

protected:
    float padding(const Theme &theme) const override { return std::max(theme.padding(), padding_); }
};

float toLinear(float c)
{
    return std::pow(c, 2.2f);
}

using Clock = std::chrono::steady_clock;

// Milliseconds since `start`
double msSince(Clock::time_point start)
{
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

}

Context::Context(engine::Window &window, engine::Renderer &renderer, const Theme &theme)
    : window_(window), renderer_(renderer), theme_(theme), root_(std::make_unique<Root>()), uiRenderer_(renderer, theme_.font())
{
    signals_.readClipboard = [this] { return window_.clipboardText(); };
    signals_.writeClipboard = [this](const std::string &text) { window_.setClipboardText(text); };
}

Context::~Context()
{
    // The UI renderer's GPU objects may still be in use by frames in flight
    renderer_.waitIdle();
}

Rect Context::bounds() const
{
    int width = 0;
    int height = 0;
    window_.framebufferSize(width, height);
    return {0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height)};
}

void Context::closeOverlay(const Component &overlay)
{
    for (Overlay &o : overlays_) {
        if (o.component.get() == &overlay)
            o.closing = true;
    }
}

bool Context::isOpen(const Component &overlay) const
{
    for (const Overlay &o : overlays_) {
        if (o.component.get() == &overlay && !o.closing)
            return true;
    }
    return false;
}

bool Context::overlayCovers(const Rect &rect) const
{
    for (const Overlay &o : overlays_) {
        if (o.closing)
            continue;
        const Rect r = o.component->rect();
        const float x0 = std::max(r.x, rect.x), y0 = std::max(r.y, rect.y);
        const float x1 = std::min(r.x + r.w, rect.x + rect.w), y1 = std::min(r.y + r.h, rect.y + rect.h);
        if (x1 <= x0 || y1 <= y0)
            continue;
        // Only one that takes the mouse there (a hidden overlay, such as a put-away browser, doesn't)
        if (o.component->hitTest({(x0 + x1) * 0.5f, (y0 + y1) * 0.5f}) || o.options.modal)
            return true;
    }
    return false;
}

void Context::bringToFront(const Component &overlay)
{
    // Only past other non-modal overlays that stay open on outside clicks, so menus and dialogs
    // stay on top. Deferred, since this usually runs during input.
    post([this, &overlay] {
        auto it = std::find_if(overlays_.begin(), overlays_.end(),
                               [&](const Overlay &o) { return o.component.get() == &overlay; });
        if (it == overlays_.end())
            return;
        Overlay moving = std::move(*it);
        overlays_.erase(it);
        auto at = overlays_.end();
        while (at != overlays_.begin()) {
            const Overlay &below = *(at - 1);
            if (!below.options.modal && !below.options.closeOnOutsideClick)
                break;
            --at;
        }
        overlays_.insert(at, std::move(moving));
    });
}

void Context::removeClosedOverlays()
{
    // onClose may open or close other overlays, so collect first
    std::vector<Overlay> closed;
    for (auto it = overlays_.begin(); it != overlays_.end();) {
        if (it->closing) {
            closed.push_back(std::move(*it));
            it = overlays_.erase(it);
        } else {
            ++it;
        }
    }
    for (Overlay &o : closed) {
        if (o.options.onClose)
            o.options.onClose();
    }
}

void Context::layout()
{
    const Rect window = bounds();
    LayoutPass pass; // each component measured once
    root_->arrange(window, theme_);
    for (Overlay &o : overlays_)
        o.component->arrange(window, theme_);
}

void Context::readMouse(std::vector<engine::InputEvent> &events, Input &input)
{
    double x = 0.0;
    double y = 0.0;
    window_.cursorPosition(x, y);
    input.mouse = {static_cast<float>(x) * scaleX_, static_cast<float>(y) * scaleY_};

    // Button events in order. A button pressed and released within one frame shows as pressed
    // now and released next frame, so a click is never missed; events after that wait too.
    std::vector<engine::InputEvent> keyboard;
    bool pressedLeft = false;
    bool pressedRight = false;
    bool holding = false;
    for (size_t i = 0; i < events.size(); ++i) {
        engine::InputEvent &e = events[i];
        if (holding) {
            if (std::holds_alternative<engine::KeyEvent>(e) || std::holds_alternative<engine::TextEvent>(e))
                keyboard.push_back(std::move(e));
            else
                carried_.push_back(std::move(e));
            continue;
        }
        if (auto *button = std::get_if<engine::MouseButtonEvent>(&e)) {
            engine::MouseButtonEvent scaled = *button;
            scaled.x *= scaleX_;
            scaled.y *= scaleY_;
            const bool left = button->button == engine::MouseButton::Left;
            const bool right = button->button == engine::MouseButton::Right;
            if (!button->down && ((left && pressedLeft) || (right && pressedRight))) {
                holding = true;
                carried_.push_back(std::move(e));
                continue;
            }
            if (signals_.mouseButton.emit(scaled))
                continue; // a hook took it; the tree doesn't see it
            if (left) {
                if (button->down && !leftDown_)
                    input.pressed = pressedLeft = true;
                if (!button->down && leftDown_)
                    input.released = true;
                leftDown_ = button->down;
            } else if (right) {
                if (button->down && !rightDown_)
                    input.secondaryPressed = pressedRight = true;
                rightDown_ = button->down;
            } else if (button->button == engine::MouseButton::Middle) {
                if (button->down && !middleDown_)
                    input.middlePressed = true;
                middleDown_ = button->down;
            }
        } else if (auto *wheel = std::get_if<engine::ScrollEvent>(&e)) {
            if (!signals_.scroll.emit(*wheel)) {
                input.scroll += static_cast<float>(wheel->y);
                input.scrollX += static_cast<float>(wheel->x);
            }
        } else if (std::holds_alternative<engine::KeyEvent>(e) || std::holds_alternative<engine::TextEvent>(e)) {
            keyboard.push_back(std::move(e));
        }
    }
    input.down = leftDown_;
    input.middleDown = middleDown_;
    events.swap(keyboard);
}

namespace {
Context *activeContext = nullptr;
}

Context *Context::active()
{
    return activeContext;
}

Context &Context::activeOr(Context &fallback)
{
    return activeContext ? *activeContext : fallback;
}

Vec2 Context::toScreen(Vec2 p) const
{
    int x = 0, y = 0, fbWidth = 0, fbHeight = 0, winWidth = 0, winHeight = 0;
    window_.position(x, y);
    window_.framebufferSize(fbWidth, fbHeight);
    window_.size(winWidth, winHeight);
    const float sx = fbWidth > 0 ? static_cast<float>(winWidth) / fbWidth : 1.0f;
    const float sy = fbHeight > 0 ? static_cast<float>(winHeight) / fbHeight : 1.0f;
    return {x + p.x * sx, y + p.y * sy};
}

Vec2 Context::fromScreen(Vec2 p) const
{
    int x = 0, y = 0, fbWidth = 0, fbHeight = 0, winWidth = 0, winHeight = 0;
    window_.position(x, y);
    window_.framebufferSize(fbWidth, fbHeight);
    window_.size(winWidth, winHeight);
    const float sx = winWidth > 0 ? static_cast<float>(fbWidth) / winWidth : 1.0f;
    const float sy = winHeight > 0 ? static_cast<float>(fbHeight) / winHeight : 1.0f;
    return {(p.x - x) * sx, (p.y - y) * sy};
}

void Context::update()
{
    const Clock::time_point start = Clock::now();
    stats_.updateMs = 0.0;
    stats_.layoutMs = 0.0;

    struct Activate {
        Context *previous;
        explicit Activate(Context *c) : previous(activeContext) { activeContext = c; }
        ~Activate() { activeContext = previous; }
    } activate(this);

    removeClosedOverlays();
    const Clock::time_point layoutStart = Clock::now();
    layout();
    stats_.layoutMs += msSince(layoutStart);

    signals_.beginFrame();

    // GLFW reports the cursor in window coordinates; layout is in framebuffer pixels
    int fbWidth = 0, fbHeight = 0, winWidth = 0, winHeight = 0;
    window_.framebufferSize(fbWidth, fbHeight);
    window_.size(winWidth, winHeight);
    scaleX_ = winWidth > 0 ? static_cast<float>(fbWidth) / winWidth : 1.0f;
    scaleY_ = winHeight > 0 ? static_cast<float>(fbHeight) / winHeight : 1.0f;

    std::vector<engine::InputEvent> events;
    events.swap(carried_);
    for (engine::InputEvent &e : window_.takeEvents())
        events.push_back(std::move(e));

    Input input;
    input.signals = &signals_;
    input.mods = window_.modifiers();
    input.shift = (input.mods & engine::ModShift) != 0;
    input.time = window_.time();
    input.dt = lastTime_ < 0.0 ? 0.0f : static_cast<float>(input.time - lastTime_);
    lastTime_ = input.time;
    engine::Cursor cursor = engine::Cursor::Arrow;
    input.cursor = &cursor;
    // The mouse and the wheel; the keyboard stays in `events`, in order, for after the tree
    readMouse(events, input);

    // Overlays first, top down. Each layer sees what the layers above leave of the input.
    // Index-based: callbacks may open new overlays, which wait until the next frame.
    Input remaining = input;
    bool blocked = false;
    const size_t count = overlays_.size();
    for (size_t i = count; i-- > 0;) {
        // By index and with the options copied: callbacks may open overlays, growing the vector
        if (overlays_[i].closing)
            continue;
        Component *overlay = overlays_[i].component.get();
        const OverlayOptions &options = overlays_[i].options;
        const bool modal = options.modal;
        const bool popup = options.closeOnOutsideClick;
        if (blocked) {
            overlay->handleInput(remaining.occluded());
            continue;
        }

        const bool inside = overlay->hitTest(remaining.mouse);
        const bool outsidePress = !inside && (remaining.pressed || remaining.secondaryPressed);
        if (outsidePress && popup) {
            // The press closes this overlay and goes no further
            overlays_[i].closing = true;
            overlay->handleInput(remaining.occluded());
            remaining.pressed = remaining.secondaryPressed = false;
        } else {
            overlay->handleInput(remaining);
        }

        if (modal) {
            blocked = true;
            continue;
        }
        if (inside) {
            // Covered: nothing below is hovered or newly pressed here. A drag already under
            // way below keeps the real mouse, so it doesn't jump.
            Input below = remaining.occluded();
            below.down = remaining.down;
            if (remaining.down)
                below.mouse = remaining.mouse;
            remaining = below;
        }
    }
    root_->handleInput(blocked ? remaining.occluded() : remaining);
    window_.setCursor(cursor);

    // The keyboard, now that the tree has seen this frame's clicks (a click on a text field
    // focuses it before the keys typed after it)
    for (const engine::InputEvent &e : events) {
        if (const auto *key = std::get_if<engine::KeyEvent>(&e))
            signals_.dispatch(*key);
        else if (const auto *text = std::get_if<engine::TextEvent>(&e))
            signals_.dispatch(*text);
    }

    // Changes to the tree and windows, now that nothing is iterating it
    std::vector<std::function<void()>> actions;
    actions.swap(posted_);
    for (auto &action : actions)
        action();
    removeClosedOverlays();

    // Neumorphism only works when the background is exactly the surface colour.
    // Set after input, since a callback may have changed the theme; the clear colour is linear.
    const Color base = theme_.baseColor();
    renderer_.setClearColor(toLinear(base.r), toLinear(base.g), toLinear(base.b));
    stats_.updateMs = msSince(start);
}

uint64_t Context::buildFrame()
{
    const Clock::time_point start = Clock::now();
    // What submit records is unknown until it runs: the frame counts as not presented
    stats_.submitMs = 0.0;
    stats_.draws = 0;
    stats_.pipelineBinds = 0;
    stats_.uploadBytes = 0;
    stats_.presented = false;

    // Lay out again in case a callback changed the theme, the tree or the overlays
    const Clock::time_point layoutStart = Clock::now();
    layout();
    stats_.layoutMs += msSince(layoutStart);
    // The theme may have changed this frame: text is laid out with its font
    drawList_.setFont(&theme_.font());
    drawList_.setBounds(bounds());
    drawList_.clear();
    root_->draw(drawList_, theme_);
    for (const Overlay &o : overlays_) {
        if (o.options.modal)
            drawList_.add(theme_.scrim(bounds(), 0.6f));
        o.component->draw(drawList_, theme_);
    }
    const DrawList::Counts &counts = drawList_.counts();
    stats_.surfaces = counts.surfaces;
    stats_.texts = counts.texts;
    stats_.glyphs = counts.glyphs;
    stats_.images = counts.images;
    stats_.lines = counts.lines;
    stats_.plots = counts.plots;
    stats_.plotValues = drawList_.plotData().size();
    stats_.batches = 0; // batches are gone: the list is drawn as runs of prims

    // The clear colour is part of the picture too (update sets it from the theme)
    Fingerprint clear;
    clear.add(theme_.baseColor());
    const uint64_t print = fingerprint(drawList_, clear.value());
    stats_.buildMs = msSince(start);
    return print;
}

void Context::submit()
{
    uiRenderer_.render(drawList_);
    // What the renderer recorded (its time, draws, binds and upload; glyphs are counted in buildFrame)
    const FrameStats &recorded = uiRenderer_.stats();
    stats_.submitMs = recorded.submitMs;
    stats_.draws = recorded.draws;
    stats_.pipelineBinds = recorded.pipelineBinds;
    stats_.uploadBytes = recorded.uploadBytes;
}

void Context::render()
{
    buildFrame();
    submit();
}

}
