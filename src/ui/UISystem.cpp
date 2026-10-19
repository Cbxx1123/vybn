#include "UISystem.h"

#include <utility>

namespace ui {


UISystem::UISystem(env::Environment &env, const std::string &title, uint32_t width, uint32_t height, const Theme &theme,
                   bool startHidden)
    : env::Daemon("UI"), env_(env), windowSystem_(env.require<engine::WindowSystem>("UISystem"))
{
    closingSubscription_ = env.events().subscribe<engine::WindowClosing>(
        [this](const engine::WindowClosing &event) { onWindowClosing(event.id); });
    // The main window waits for vsync, which paces the whole loop
    hidden_ = startHidden;
    create(title, width, height, theme, true, false, startHidden);
}

UISystem::~UISystem()
{
    env_.events().unsubscribe(closingSubscription_);
    // Contexts go here, before the WindowSystem (added earlier, destroyed later) destroys the
    // renderers they draw with. Newest first.
    while (!windows_.empty())
        windows_.pop_back();
}

UISystem::UIWindow &UISystem::create(const std::string &title, uint32_t width, uint32_t height, const Theme &theme,
                                     bool main, bool appPlaced, bool hidden)
{
    engine::WindowOptions options;
    // Extra windows don't wait for vsync, so they never slow the main one down
    options.vsync = main;
    options.visible = !appPlaced && !hidden;
    // A tool window (or a spare for one) is sized when it is used: its swapchain waits until then
    options.deferSwapchain = appPlaced;
    const engine::WindowId id = windowSystem_.open(title, width, height, options);

    auto w = std::make_unique<UIWindow>();
    w->id = id;
    engine::Window &window = windowSystem_.window(id);
    w->context = std::make_unique<Context>(window, windowSystem_.renderer(id), theme);
    w->context->setKeymap(&keymap_);
    // Actions that nothing in an extra window handles go to the main window (the app's commands)
    if (!main && !windows_.empty())
        w->context->signals().setFallback(&windows_.front()->context->signals());

    Container &root = w->context->root();
    w->titleBar = &root.add<TitleBar>(window);
    w->body = &root.add<Container>(Direction::Column);
    w->body->setAlign(Align::Stretch).setAlignSelf(Align::Stretch);
    w->body->setGrow(1.0f);

    // The title bar's empty parts drag the window. Window coordinates to framebuffer pixels,
    // as Context does for the mouse.
    UIWindow *raw = w.get();
    if (!appPlaced)
        windowSystem_.setCaptionTest(id, [raw, &window](double x, double y) {
            int fbWidth = 0, fbHeight = 0, winWidth = 0, winHeight = 0;
            window.framebufferSize(fbWidth, fbHeight);
            window.size(winWidth, winHeight);
            const float sx = winWidth > 0 ? static_cast<float>(fbWidth) / winWidth : 1.0f;
            const float sy = winHeight > 0 ? static_cast<float>(fbHeight) / winHeight : 1.0f;
            return raw->titleBar->isDragArea({static_cast<float>(x) * sx, static_cast<float>(y) * sy});
        });

    windows_.push_back(std::move(w));
    return *windows_.back();
}

UISystem::WindowParts UISystem::openToolWindow(const std::string &title, uint32_t width, uint32_t height,
                                               std::function<void(Container &)> onClose,
                                               std::function<bool()> onCloseRequest)
{
    UIWindow *spare = nullptr;
    for (auto &candidate : windows_) {
        if (candidate->spare)
            spare = candidate.get();
    }
    if (spare) {
        // Ready already: size it, name it and let it go
        spare->spare = false;
        engine::Window &window = windowSystem_.window(spare->id);
        window.setSize(static_cast<int>(width), static_cast<int>(height));
        window.setTitle(title);
    }
    UIWindow &w = spare ? *spare : create(title, width, height, context().theme(), false, true);
    w.onClose = std::move(onClose);
    if (onCloseRequest)
        windowSystem_.setCloseHandler(w.id, std::move(onCloseRequest));
    WindowParts parts;
    parts.id = w.id;
    parts.window = &windowSystem_.window(w.id);
    parts.context = w.context.get();
    parts.titleBar = w.titleBar;
    parts.body = w.body;
    return parts;
}

Container &UISystem::openWindow(const std::string &title, uint32_t width, uint32_t height,
                                std::function<void(Container &)> onClose)
{
    UIWindow &w = create(title, width, height, context().theme(), false);
    w.onClose = std::move(onClose);
    // Extra windows show their title in their own title bar
    w.titleBar->add<Label>(title, TextStyle::Subtitle);
    return *w.body;
}

void UISystem::onWindowClosing(engine::WindowId id)
{
    for (auto it = windows_.begin(); it != windows_.end(); ++it) {
        if ((*it)->id != id)
            continue;
        // Let the owner take content back while it still exists, then let go of the Context
        // before the WindowSystem destroys the renderer under it
        std::unique_ptr<UIWindow> closing = std::move(*it);
        windows_.erase(it);
        if (closing->onClose)
            closing->onClose(*closing->body);
        return;
    }
}

Context *UISystem::owner(const Component &overlay)
{
    for (auto &w : windows_) {
        if (w->context->isOpen(overlay))
            return w->context.get();
    }
    return nullptr;
}

void UISystem::popOut(FloatingWindow &floating)
{
    // Moving components between trees has to wait until no tree is mid-input
    post([this, &floating] {
        Context *from = owner(floating);
        if (!from)
            return;
        const std::string title = floating.title();
        const Rect frame = floating.frame();
        auto handler = floating.popOutHandler();
        std::unique_ptr<Container> content = floating.takeContent();
        from->closeOverlay(floating);

        // Where the floating window was, with room added for the native window's padding and
        // title bar, so the content gets the same space it had
        const Theme &theme = from->theme();
        const float chrome = 2.0f * theme.padding();
        const uint32_t width = static_cast<uint32_t>(frame.w + chrome);
        const uint32_t height = static_cast<uint32_t>(frame.h + chrome + theme.controlHeight(Size::Small));
        Container &body = openWindow(title, width, height, [from, title, frame, handler](Container &body) {
            // Dock back: the content returns to a floating window
            auto *scroll = static_cast<ScrollView *>(&body.child(0));
            std::unique_ptr<Container> content = scroll->takeContent();
            FloatingWindow &docked = FloatingWindow::open(*from, title, frame);
            docked.setContent(std::move(content));
            if (handler)
                docked.onPopOut(handler);
        });
        auto &scroll = body.add<ScrollView>();
        scroll.setGrow(1.0f);
        scroll.setContent(std::move(content));

        int x = 0, y = 0;
        from->window().position(x, y);
        engine::Window &window = windowSystem_.window(windows_.back()->id);
        window.setPosition(x + static_cast<int>(frame.x - theme.padding()), y + static_cast<int>(frame.y - theme.padding()));
        window.focus();
    });
}

// Spares are sized when they are taken, so they wait small: a tiny swapchain instead of a
// window's worth of images each
constexpr uint32_t kSpareWidth = 160;
constexpr uint32_t kSpareHeight = 120;

void UISystem::prepareSpare()
{
    if (env_.frame() < spareAfter_)
        return;
    // Spares and windows in use together stay within the pool's size
    if (windows_.size() > kSpares)
        return;
    UIWindow &w = create("Vibin", kSpareWidth, kSpareHeight, context().theme(), false, true);
    w.spare = true;
    spareAfter_ = env_.frame() + 20;
}

void UISystem::prepareSpares(const std::function<void()> &between)
{
    while (windows_.size() <= kSpares) {
        UIWindow &w = create("Vibin", kSpareWidth, kSpareHeight, context().theme(), false, true);
        w.spare = true;
        if (between)
            between();
    }
}

void UISystem::releaseToolWindow(engine::WindowId id)
{
    for (auto &w : windows_) {
        if (w->id != id || w == windows_.front())
            continue;
        w->onClose = nullptr;
        windowSystem_.setCloseHandler(id, nullptr);
        w->body->clear();
        w->titleBar->clear();
        windowSystem_.window(id).hide();
        w->spare = true;
        return;
    }
}

void UISystem::countFrame(double now, bool presented)
{
    if (secondStart_ < 0.0)
        secondStart_ = now;
    if (presented)
        ++secondDrawn_;
    else
        ++secondSkipped_;
    const double elapsed = now - secondStart_;
    if (elapsed >= 1.0) {
        frameRate_.drawn = static_cast<double>(secondDrawn_) / elapsed;
        frameRate_.skipped = static_cast<double>(secondSkipped_) / elapsed;
        secondDrawn_ = 0;
        secondSkipped_ = 0;
        secondStart_ = now;
    }
}

void UISystem::pace(bool presented, bool input, bool posted)
{
    using Clock = std::chrono::steady_clock;
    const auto now = Clock::now();
    // Work this tick: the loop runs again at once (vsync already paced a presented frame)
    if (presented || input || posted) {
        lastActivity_ = now;
        sleep_ = 0.0;
        return;
    }
    // Busy with work that changes without input: at least the fastest idle pace. Keeping the
    // activity time fresh holds that pace for half a second after the work ends too.
    if (busy_) {
        lastActivity_ = now;
        sleep_ = 1.0 / 60.0;
        return;
    }
    // Idle: a quick pace just after anything happened, then slower the longer nothing does
    const double quiet = std::chrono::duration<double>(now - lastActivity_).count();
    sleep_ = quiet < 0.5 ? 1.0 / 60.0 : quiet < 3.0 ? 1.0 / 30.0 : 1.0 / 10.0;
}

std::string UISystem::paceText() const
{
    if (sleep_ <= 0.0)
        return "active";
    if (busy_)
        return "busy 60/s";
    return "idle " + std::to_string(static_cast<int>(1.0 / sleep_ + 0.5)) + "/s";
}

void UISystem::tick(env::Environment &)
{
    prepareSpare();
    bool anyPresented = false;
    // Input waiting in any window (taken by its Context below)
    bool anyInput = false;
    for (const auto &w : windows_) {
        if (windowSystem_.exists(w->id) && windowSystem_.window(w->id).hasEvents())
            anyInput = true;
    }
    // By index: a window's callbacks may open another (it waits for the next tick)
    const size_t count = windows_.size();
    for (size_t i = 0; i < count && i < windows_.size(); ++i) {
        UIWindow *w = windows_[i].get();
        if (w->spare) {
            // Waiting in the pool: no swapchain until it is taken (and sized). Taken, it presents at once.
            engine::Renderer &renderer = windowSystem_.renderer(w->id);
            if (renderer.hasSwapchain())
                renderer.releaseSwapchain();
            w->presented = false;
            continue;
        }
        // Every window follows the main window's theme
        if (i != 0)
            w->context->theme() = context().theme();
        engine::Window &window = windowSystem_.window(w->id);
        if (!windowSystem_.isVisible(w->id)) {
            // Minimised: what it showed is gone, so it presents again once it is back
            w->presented = false;
            continue;
        }
        engine::Renderer &renderer = windowSystem_.renderer(w->id);
        w->context->update();
        // Lay out and fill the frame; the picture is presented only when it differs from the one on screen
        const uint64_t print = w->context->buildFrame();
        int fbWidth = 0;
        int fbHeight = 0;
        window.framebufferSize(fbWidth, fbHeight);
        // The window was exposed or restored, asked for a redraw, or its swapchain was replaced
        const bool refreshed = window.takeRefreshRequested();
        const bool requested = w->context->takeRedrawRequest();
        const bool changed = !w->presented || print != w->print || fbWidth != w->fbWidth || fbHeight != w->fbHeight ||
                             renderer.swapchainGeneration() != w->generation || refreshed || requested ||
                             (i == 0 && hidden_); // the hidden main window draws until it has shown
        if (changed) {
            renderer.beginFrame();
            w->context->submit();
            renderer.endFrame();
            reportWait(renderer.lastWaitMs());
            w->presented = true;
            w->print = print;
            w->fbWidth = fbWidth;
            w->fbHeight = fbHeight;
            w->generation = renderer.swapchainGeneration();
            ++w->presentedCount;
            anyPresented = true;
            // The main window shows once it has something on it
            if (i == 0 && hidden_ && ++drawnFrames_ >= 2) {
                hidden_ = false;
                window.show();
                if (onShown_)
                    onShown_();
            }
        } else {
            ++w->skippedCount;
        }
        if (i == 0)
            countFrame(window.time(), changed);
    }

    std::vector<std::function<void()>> actions;
    actions.swap(posted_);
    for (auto &action : actions)
        action();

    // How long the next loop may sleep: a window's vsync paces a presented frame, and input,
    // posted actions and other threads' wakes (see Environment::setWakeHook) end a sleep at once
    pace(anyPresented, anyInput, !actions.empty());
    windowSystem_.setNextWait(sleep_);
}

}
