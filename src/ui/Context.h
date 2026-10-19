#pragma once

#include <functional>
#include <memory>
#include <utility>
#include <vector>

#include "Components.h"
#include "Controls.h"
#include "DrawList.h"
#include "FrameStats.h"
#include "Layout.h"
#include "Navigation.h"
#include "Theme.h"
#include "UIRenderer.h"
#include "engine/Renderer.h"
#include "engine/Window.h"

namespace ui {

// How an overlay behaves (see Context::openOverlay)
struct OverlayOptions {
    bool modal = false;              // veils and blocks everything beneath it (dialogs)
    bool closeOnOutsideClick = true; // a press outside it closes it and is used up (menus, popups)
    std::function<void()> onClose;   // runs when it closes, however that happens
};

// Entry point for the UI in one window: owns the theme, the root container, the overlays and
// the UI renderer. Each frame: update() before Renderer::beginFrame, render() between beginFrame
// and endFrame. Must be destroyed before the Renderer and Window it was given.
class Context {
public:
    Context(engine::Window &window, engine::Renderer &renderer, const Theme &theme = Theme::light());
    ~Context();

    Context(const Context &) = delete;
    Context &operator=(const Context &) = delete;

    // Fills the window and centres its children by default
    Container &root() { return *root_; }
    // The window's input as signals (keys, text, actions, mouse, wheel), for anything to hook
    InputSignals &signals() { return signals_; }
    // Which keys run which actions; UISystem gives every window the same one
    Keymap &keymap() { return signals_.keymap(); }
    void setKeymap(Keymap *keymap) { signals_.setKeymap(keymap); }
    Theme &theme() { return theme_; }
    engine::Window &window() { return window_; }
    // The window's area in framebuffer pixels
    Rect bounds() const;
    // Converts between this window's framebuffer pixels and the desktop's screen coordinates
    Vec2 toScreen(Vec2 p) const;
    Vec2 fromScreen(Vec2 p) const;
    // The Context running update() right now (handling input, running callbacks and its posted
    // actions), or null. Popups open in it, so a menu opened from content that moved into
    // another window shows in that window.
    static Context *active();
    // `fallback`, or the active Context when there is one
    static Context &activeOr(Context &fallback);

    // Overlays sit above the root, newest on top: menus, dialogs, floating windows. Each is
    // arranged with the whole window's rect and places itself inside it. The topmost overlay
    // under the mouse gets the mouse; what it covers doesn't. Opening a modal overlay or one that
    // closes on outside clicks takes the keyboard focus away; they keep keys from what is under
    // them with their own hooks (Priority::Modal, Priority::Popup).
    template <typename T, typename... Args>
    T &openOverlay(OverlayOptions options, Args &&...args)
    {
        if (options.modal || options.closeOnOutsideClick)
            signals_.setFocus(nullptr);
        auto overlay = std::make_unique<T>(std::forward<Args>(args)...);
        T &ref = *overlay;
        overlays_.push_back({std::move(overlay), std::move(options), false});
        return ref;
    }
    // Closes an overlay at the end of the current frame (safe to call from its own callbacks)
    void closeOverlay(const Component &overlay);
    bool isOpen(const Component &overlay) const;
    // Raises a non-modal overlay above the others (floating windows when clicked)
    void bringToFront(const Component &overlay);
    // Whether an open overlay that takes the mouse covers part of `rect` (a menu over a view)
    bool overlayCovers(const Rect &rect) const;

    // Runs `action` after this frame's input: the place to add or remove components, move them
    // between windows, or open and close windows
    void post(std::function<void()> action) { posted_.push_back(std::move(action)); }

    // Lays out the tree, takes the window's input events and runs callbacks: mouse and wheel
    // through the signals and then the tree, then keys and text through the signals
    void update();
    // Records the UI into the current frame: buildFrame, then submit
    void render();
    // Lays out the tree and fills the DrawList, with no GPU work. Returns the fingerprint of what
    // it drew (see fingerprint in DrawList.h, with the clear colour mixed in): equal fingerprints
    // mean the same picture, so the frame need not be presented again.
    uint64_t buildFrame();
    // Records the DrawList from the last buildFrame into the current frame (between beginFrame and endFrame)
    void submit();
    // What the last update, buildFrame and submit cost and drew. buildFrame sets submit's fields
    // (submitMs, glyphs, draws, pipelineBinds, uploadBytes) and `presented` back to 0 and false;
    // submit fills them, so they are 0 for a frame skipped as unchanged.
    const FrameStats &stats() const { return stats_; }
    // Called by whoever presents (UISystem) after deciding: true when the last buildFrame was
    // submitted, false when it was skipped as unchanged
    void setPresented(bool presented) { stats_.presented = presented; }

    // Presents the window's next frame even when its picture hasn't changed
    void requestRedraw() { redrawRequested_ = true; }
    // Whether requestRedraw was called since the last time this was asked (and clears it)
    bool takeRedrawRequest()
    {
        const bool requested = redrawRequested_;
        redrawRequested_ = false;
        return requested;
    }

private:
    struct Overlay {
        std::unique_ptr<Component> component;
        OverlayOptions options;
        bool closing;
    };

    void layout();
    void removeClosedOverlays();

    // The mouse state built from this frame's events
    void readMouse(std::vector<engine::InputEvent> &events, Input &input);

    engine::Window &window_;
    engine::Renderer &renderer_;
    InputSignals signals_;
    std::vector<engine::InputEvent> carried_; // events held back to the next frame
    Theme theme_;
    std::unique_ptr<Container> root_;
    std::vector<Overlay> overlays_;
    std::vector<std::function<void()>> posted_;
    UIRenderer uiRenderer_;
    DrawList drawList_;
    bool redrawRequested_ = false;
    FrameStats stats_;
    bool leftDown_ = false;
    bool rightDown_ = false;
    bool middleDown_ = false;
    float scaleX_ = 1.0f, scaleY_ = 1.0f; // window coordinates to framebuffer pixels
    double lastTime_ = -1.0;
};

}
