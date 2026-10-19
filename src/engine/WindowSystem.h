#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "Renderer.h"
#include "Window.h"
#include "env/Daemon.h"
#include "env/Environment.h"

namespace engine {

using WindowId = uint32_t;

struct WindowOptions {
    // Waits for the display between frames. Extra windows usually turn it off so they don't
    // slow the main one down (see Renderer).
    bool vsync = true;
    // The app draws its own title bar (see Window::useCustomTitleBar and setCaptionTest)
    bool customTitleBar = true;
    // Shown at once; a hidden window shows with Window::show (after placing it, say)
    bool visible = true;
    // No swapchain until the window is first drawn: a hidden window waiting to be used costs
    // its images and the driver's presentation state only once it is (see Renderer)
    bool deferSwapchain = false;
};

// Events published on the Environment's bus
struct WindowOpened {
    WindowId id;
};
// Published right before a window and its renderer are destroyed, so whatever draws into it
// (a UI Context) can let go first
struct WindowClosing {
    WindowId id;
};

// The windowing daemon: owns every native window and its renderer, and pumps the OS event
// queue. The first window opened is the main window; when the user closes it the Environment
// stops. Other windows are destroyed when closed, after a WindowClosing event.
//
// Each tick polls events, or sleeps briefly when every window is minimised, then handles windows
// that were closed. Daemons that draw tick after this one.
class WindowSystem : public env::Daemon {
public:
    explicit WindowSystem(env::Environment &env);
    ~WindowSystem() override;

    WindowId open(const std::string &title, uint32_t width, uint32_t height, const WindowOptions &options = {});
    // Closes a window at the next tick, as if the user had closed it
    void close(WindowId id);

    bool exists(WindowId id) const { return find(id) != nullptr; }
    Window &window(WindowId id);
    Renderer &renderer(WindowId id);
    // Which points (window coordinates) drag the window, for windows with a custom title bar
    void setCaptionTest(WindowId id, std::function<bool(double x, double y)> isCaption);
    // Asked when the user closes a window (not when close() is called); returning false keeps it
    // open, to close it some other way (after an animation, say)
    void setCloseHandler(WindowId id, std::function<bool()> handler);
    // False while minimised: there's nothing to draw into
    bool isVisible(WindowId id) const;

    WindowId mainWindow() const { return main_; }
    std::vector<WindowId> windows() const;
    size_t count() const { return windows_.size(); }

    // How long the next tick may sleep waiting for events (input, or a wake from another thread),
    // set by the UI each tick: zero polls at once. Applies to the next tick only, then resets.
    void setNextWait(double seconds) { nextWait_ = seconds; }

    void start(env::Environment &env) override;
    void tick(env::Environment &env) override;
    void stop(env::Environment &env) override;

private:
    struct Entry {
        // Declared in creation order, so the renderer goes before its window
        WindowId id = 0;
        std::unique_ptr<Window> window;
        std::unique_ptr<Renderer> renderer;
        std::function<bool(double, double)> isCaption;
        std::function<bool()> onCloseRequest;
        bool closeRequested = false;
    };
    Entry *find(WindowId id) const;
    Entry &get(WindowId id) const;

    env::Environment &env_;
    std::vector<std::unique_ptr<Entry>> windows_;
    WindowId main_ = 0;
    WindowId next_ = 1;
    double nextWait_ = 0.0;
};

}
