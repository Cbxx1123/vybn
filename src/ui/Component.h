#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "DrawList.h"
#include "Geometry.h"
#include "Signals.h"
#include "Theme.h"
#include "engine/Window.h"

namespace ui {

using engine::Chord;
using engine::Key;
using engine::KeyEvent;
using engine::TextEvent;

// The mouse for one frame, with positions in framebuffer pixels. The keyboard isn't here: it
// arrives through `signals`, the window's InputSignals, which components hook (see Hooks).
// Presses come from the window's events, so a click that is pressed and released between two
// frames still shows as a press, then a release on the next frame.
struct Input {
    Vec2 mouse;
    bool down = false;     // left button held
    bool pressed = false;  // went down this frame
    bool released = false; // went up this frame
    // Right button went down this frame. The innermost ContextMenuArea uses it up.
    mutable bool secondaryPressed = false;
    // Middle button (the wheel pressed): held, and went down this frame. Views that pan with it
    // use the press up, so only one starts panning.
    bool middleDown = false;
    mutable bool middlePressed = false;
    // Mouse wheel lines this frame (positive scrolls up). The innermost ScrollView under the
    // mouse sets it to 0 once it has used it, so outer scroll views don't scroll as well.
    mutable float scroll = 0.0f;
    mutable float scrollX = 0.0f; // sideways wheel or trackpad (positive scrolls left)
    bool shift = false;           // Shift held
    uint8_t mods = 0;             // modifiers held (engine::Mod bits)
    // The window's signals, for hooking keys, text and actions. Null in input that isn't meant
    // for the component (a closed drawer's content), which then gets no keyboard.
    InputSignals *signals = nullptr;
    double time = 0.0;     // seconds, for animation and caret blink
    float dt = 0.0f;       // seconds since the previous frame
    // The pointer's shape this frame (the arrow unless something under the mouse asks for
    // another; the last to ask wins). Null in occluded input, so covered things can't set it.
    engine::Cursor *cursor = nullptr;
    void setCursor(engine::Cursor shape) const
    {
        if (cursor)
            *cursor = shape;
    }

    // This input as seen by something covered up: no hover, no new presses. Releases and timing
    // still arrive, so held states end and animations keep running, and the signals stay (what
    // covers it keeps the keys from it with its own hooks).
    Input occluded() const
    {
        Input in;
        in.mouse = {-1e9f, -1e9f};
        in.released = released;
        in.middleDown = middleDown;
        in.time = time;
        in.dt = dt;
        in.mods = mods;
        in.shift = shift;
        in.signals = signals;
        return in;
    }

    // Smoothing factor that eases a value toward its target at the same speed at any frame rate
    float ease(float rate = 18.0f) const { return 1.0f - std::exp(-rate * std::min(dt, 0.1f)); }
};

// Base of every UI element. Each frame: measure/arrange (layout), handleInput, then draw.
class Component {
public:
    virtual ~Component() = default;

    // Preferred size; containers may give more (grow, Align::Stretch)
    virtual Vec2 measure(const Theme &theme) const = 0;
    virtual void arrange(const Rect &rect, const Theme &) { rect_ = rect; }
    virtual void handleInput(const Input &) {}
    virtual void draw(DrawList &, const Theme &) const {}
    // Whether `p` is on this component, for routing input between overlapping layers
    virtual bool hitTest(Vec2 p) const { return rect_.contains(p); }

    // Share of leftover space along the parent's main axis (0 = keep preferred size)
    Component &setGrow(float grow)
    {
        grow_ = grow;
        return *this;
    }
    // Overrides the parent's cross-axis alignment for this component
    Component &setAlignSelf(Align align)
    {
        alignSelf_ = align;
        return *this;
    }
    Component &setEnabled(bool enabled)
    {
        enabled_ = enabled;
        return *this;
    }
    // Grows the preferred size to at least this (0 leaves an axis alone)
    Component &setMinSize(Vec2 size)
    {
        minSize_ = size;
        return *this;
    }

    float grow() const { return grow_; }
    std::optional<Align> alignSelf() const { return alignSelf_; }
    bool enabled() const { return enabled_; }
    const Rect &rect() const { return rect_; }
    // measure() raised to the minimum size; containers lay children out with this. During a
    // layout pass (LayoutPass) each component is measured once and the result reused, so nested
    // containers don't measure their subtrees again at every level.
    Vec2 preferredSize(const Theme &theme) const
    {
        Vec2 size;
        if (layoutPass() != 0 && measuredPass_ == layoutPass() && measuredTheme_ == &theme) {
            size = measured_;
        } else {
            size = measure(theme);
            if (layoutPass() != 0) {
                measured_ = size;
                measuredPass_ = layoutPass();
                measuredTheme_ = &theme;
            }
        }
        return {std::max(size.x, minSize_.x), std::max(size.y, minSize_.y)};
    }

    // The layout pass under way (0 when none): Context::layout opens one with a LayoutPass. Sizes
    // measured in it are kept until it ends, so nothing may change what a component measures
    // while one is open (layout doesn't; input and posted actions run between passes).
    static uint64_t &layoutPass()
    {
        static uint64_t pass = 0;
        return pass;
    }

protected:
    Rect rect_;
    mutable Vec2 measured_;
    mutable uint64_t measuredPass_ = 0;
    mutable const Theme *measuredTheme_ = nullptr;
    float grow_ = 0.0f;
    std::optional<Align> alignSelf_;
    bool enabled_ = true;
    Vec2 minSize_;
};

// Opens a layout pass for as long as it lives (see Component::preferredSize); nested ones join
// the outer pass
class LayoutPass {
public:
    LayoutPass() : outer_(Component::layoutPass() != 0)
    {
        static uint64_t next = 0;
        if (!outer_)
            Component::layoutPass() = ++next;
    }
    ~LayoutPass()
    {
        if (!outer_)
            Component::layoutPass() = 0;
    }
    LayoutPass(const LayoutPass &) = delete;
    LayoutPass &operator=(const LayoutPass &) = delete;

private:
    bool outer_;
};

// Hover and press tracking shared by clickable components
struct ClickState {
    bool hovered = false;
    bool pressed = false; // pressed inside and not yet released

    // Returns true when a click (press and release both inside `area`) completes this frame
    bool update(const Rect &area, const Input &input, bool enabled)
    {
        if (!enabled) {
            hovered = pressed = false;
            return false;
        }
        hovered = area.contains(input.mouse);
        if (input.pressed && hovered)
            pressed = true;
        bool clicked = false;
        if (input.released) {
            clicked = pressed && hovered;
            pressed = false;
        }
        return clicked;
    }

    bool held() const { return pressed && hovered; }
};

}
