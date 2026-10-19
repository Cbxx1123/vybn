#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "Context.h"
#include "Dialogs.h"
#include "engine/WindowSystem.h"
#include "env/Daemon.h"
#include "env/Environment.h"

namespace ui {

// The UI daemon: a Context for each window it opens through the WindowSystem, each with the
// app's own TitleBar over a body() column. Every window shares the main window's theme, copied
// each tick. Requires an engine::WindowSystem added before it; its constructor opens the main
// window, so the UI can be built straight away.
//
// Each tick lays out, handles input and draws every visible window, then runs posted actions.
class UISystem : public env::Daemon {
public:
    // With `startHidden` the main window stays hidden until it has drawn its first frames (so it
    // never shows blank), then shows and runs the handler given to onShown
    UISystem(env::Environment &env, const std::string &title, uint32_t width, uint32_t height,
             const Theme &theme = Theme::light(), bool startHidden = false);
    void onShown(std::function<void()> handler) { onShown_ = std::move(handler); }
    // Makes the pool of spare tool windows now (about a third of a second each), rather than
    // during the first frames: for a loading screen to cover
    void prepareSpares(const std::function<void()> &between = {});
    ~UISystem() override;

    Context &context() { return *windows_.front()->context; }
    engine::Window &window() { return windowSystem_.window(windows_.front()->id); }
    TitleBar &titleBar() { return *windows_.front()->titleBar; }
    // The main window's area under its title bar: a column that fills it
    Container &body() { return *windows_.front()->body; }
    env::Environment &environment() { return env_; }
    // The keymap every window's signals use: which keys run which actions
    Keymap &keymap() { return keymap_; }

    // Opens another native window and returns its body. `onClose` runs when it is closed, just
    // before it is destroyed, while its components still exist.
    Container &openWindow(const std::string &title, uint32_t width, uint32_t height,
                          std::function<void(Container &body)> onClose = {});
    // Moves a floating window's content into a native window of the same size and place, and
    // closes the floating one. Closing the native window docks the content back into a new
    // floating window. Use it as a FloatingWindow's onPopOut handler.
    void popOut(FloatingWindow &floating);

    // A window whose place the app looks after: it opens hidden (place it, then show it), its
    // title bar doesn't drag it (the app reads the mouse and moves it, so it keeps drawing while
    // it moves, which a system drag would pause), and when the user closes it `onCloseRequest`
    // decides whether it goes (return false to keep it, and close it with closeWindow later).
    // `onClose` runs as it goes, as for openWindow.
    struct WindowParts {
        engine::WindowId id = 0;
        engine::Window *window = nullptr;
        Context *context = nullptr;
        TitleBar *titleBar = nullptr;
        Container *body = nullptr;
    };
    WindowParts openToolWindow(const std::string &title, uint32_t width, uint32_t height,
                               std::function<void(Container &body)> onClose, std::function<bool()> onCloseRequest);
    // Closes a window opened here at the next tick, running its onClose
    void closeWindow(engine::WindowId id) { windowSystem_.close(id); }
    // Done with a tool window: it empties, hides and goes back to the pool of spares, so the
    // next one opens at once (making a window takes a third of a second, and so does closing
    // one). Its onClose does not run. Call it outside input and drawing.
    void releaseToolWindow(engine::WindowId id);
    bool windowExists(engine::WindowId id) const { return windowSystem_.exists(id); }
    size_t windowCount() const { return windows_.size(); }

    // Runs `action` after this tick, once every window has handled its input
    void post(std::function<void()> action) { posted_.push_back(std::move(action)); }

    // How many frames a second the main window presented, and how many it skipped because its
    // picture hadn't changed (over the last whole second)
    struct FrameRate {
        double drawn = 0.0;
        double skipped = 0.0;
    };
    FrameRate frameRate() const { return frameRate_; }
    // The main window's last frame: its costs and counts (see FrameStats)
    const FrameStats &frameStats() const { return windows_.front()->context->stats(); }

    // Keeps the loop at its fastest idle pace (60 a second) while the app has work going on that
    // changes without input (transport playing or recording, an export, a download...). DawApp
    // sets it each frame.
    void setBusy(bool busy) { busy_ = busy; }
    // How the loop is paced now, for Settings > Metrics: "active" while it runs flat out or
    // presents every frame, else "idle 10/s" (the rate it sleeps at)
    std::string paceText() const;

    void tick(env::Environment &env) override;

private:
    struct UIWindow {
        engine::WindowId id = 0;
        std::unique_ptr<Context> context;
        TitleBar *titleBar = nullptr;
        Container *body = nullptr;
        std::function<void(Container &)> onClose;
        bool spare = false; // made ahead of time for the next tool window; not drawn until claimed
        // What is on screen: whether the window has presented a picture since it was last visible,
        // its fingerprint, framebuffer size and swapchain generation when it did
        bool presented = false;
        uint64_t print = 0;
        int fbWidth = 0;
        int fbHeight = 0;
        uint64_t generation = 0;
        uint64_t presentedCount = 0; // frames presented, and skipped as unchanged, for this window
        uint64_t skippedCount = 0;
    };

    UIWindow &create(const std::string &title, uint32_t width, uint32_t height, const Theme &theme, bool main,
                     bool appPlaced = false, bool hidden = false);
    void onWindowClosing(engine::WindowId id);
    // Context of the window showing `overlay`
    Context *owner(const Component &overlay);

    // Keeps a few hidden tool windows ready, made a little after start-up: making a window (its
    // Vulkan device, pipelines and font atlas) takes a third of a second, which would show as a
    // freeze when a panel pops out
    void prepareSpare();
    // Counts one tick of the main window, presented or skipped, into the frame rate
    void countFrame(double now, bool presented);
    // With VIBIN_FRAME_STATS set (any value but 0), adds the main window's frame to a running
    // total and once a second prints its averages to stderr, one line:
    // "FRAME 60.0/s presented 59.0/s | update 0.41 ms (layout 0.30) build 1.20 ms submit 0.35 ms |
    //  surfaces 640 texts 95 glyphs 480 images 0 lines 0 plots 0 (0 values) batches 0 |
    //  caches 12 hit 0 missed (1450 prims reused), layout skipped 98% | draws 1 binds 1 upload 88 KB"
    // (counts are per frame, averaged over the frames that had them: submit's over presented frames)
    void reportStats(double now);
    // Decides how long the loop may sleep after this tick (see tick), and tells the WindowSystem
    void pace(bool presented, bool input, bool posted);

    env::Environment &env_;
    engine::WindowSystem &windowSystem_;
    uint64_t spareAfter_ = 30; // frame to make the next spare on
    bool hidden_ = false;       // the main window, until its first frames are drawn
    int drawnFrames_ = 0;
    std::function<void()> onShown_;
    static constexpr size_t kSpares = 3;
    Keymap keymap_; // before the windows, so it outlives their Contexts
    std::vector<std::unique_ptr<UIWindow>> windows_; // [0] is the main window
    std::vector<std::function<void()>> posted_;
    env::SubscriptionId closingSubscription_ = 0;
    FrameRate frameRate_;
    uint64_t secondDrawn_ = 0;
    uint64_t secondSkipped_ = 0;
    double secondStart_ = -1.0; // window time the current count began, or -1 before the first tick
    // VIBIN_FRAME_STATS: whether it is on (read once), and the sums since the last line
    int statsEnabled_ = -1;
    double statsStart_ = -1.0;
    FrameStats statsSum_;
    uint64_t statsFrames_ = 0;
    uint64_t statsPresented_ = 0;
    uint64_t statsLayoutSkipped_ = 0; // frames whose update skipped its layout
    // Loop pacing: the last time something happened (a presented frame, input, a posted action),
    // the sleep chosen last, and whether the app is busy
    std::chrono::steady_clock::time_point lastActivity_ = std::chrono::steady_clock::now();
    double sleep_ = 0.0;
    bool busy_ = false;
};

}
