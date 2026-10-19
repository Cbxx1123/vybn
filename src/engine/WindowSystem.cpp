#include "WindowSystem.h"

#include <stdexcept>

namespace engine {

WindowSystem::WindowSystem(env::Environment &env)
    : env::Daemon("Windowing"), env_(env)
{
}

WindowSystem::~WindowSystem()
{
    // Extra windows first, then the main one
    while (!windows_.empty())
        windows_.pop_back();
}

WindowSystem::Entry *WindowSystem::find(WindowId id) const
{
    for (const auto &e : windows_) {
        if (e->id == id)
            return e.get();
    }
    return nullptr;
}

WindowSystem::Entry &WindowSystem::get(WindowId id) const
{
    Entry *e = find(id);
    if (!e)
        throw std::runtime_error("WindowSystem: no window " + std::to_string(id));
    return *e;
}

WindowId WindowSystem::open(const std::string &title, uint32_t width, uint32_t height, const WindowOptions &options)
{
    auto e = std::make_unique<Entry>();
    e->id = next_++;
    e->window = std::make_unique<Window>(width, height, title, options.visible);

    // The caption test is looked up on every hit test, so it can be set after opening.
    // Set up before the renderer, so the swapchain starts at the final size.
    if (options.customTitleBar) {
        Entry *raw = e.get();
        e->window->useCustomTitleBar([raw](double x, double y) { return raw->isCaption && raw->isCaption(x, y); });
    }
    e->renderer = std::make_unique<Renderer>(*e->window, options.vsync, options.deferSwapchain);

    const WindowId id = e->id;
    if (main_ == 0)
        main_ = id;
    windows_.push_back(std::move(e));
    env_.events().publish(WindowOpened{id});
    return id;
}

void WindowSystem::close(WindowId id)
{
    if (Entry *e = find(id))
        e->closeRequested = true;
}

Window &WindowSystem::window(WindowId id)
{
    return *get(id).window;
}

Renderer &WindowSystem::renderer(WindowId id)
{
    return *get(id).renderer;
}

void WindowSystem::setCaptionTest(WindowId id, std::function<bool(double, double)> isCaption)
{
    get(id).isCaption = std::move(isCaption);
}

void WindowSystem::setCloseHandler(WindowId id, std::function<bool()> handler)
{
    get(id).onCloseRequest = std::move(handler);
}

bool WindowSystem::isVisible(WindowId id) const
{
    const Entry *e = find(id);
    if (!e)
        return false;
    int width = 0;
    int height = 0;
    e->window->framebufferSize(width, height);
    return width > 0 && height > 0;
}

std::vector<WindowId> WindowSystem::windows() const
{
    std::vector<WindowId> ids;
    for (const auto &e : windows_)
        ids.push_back(e->id);
    return ids;
}

void WindowSystem::start(env::Environment &)
{
    // A post from another thread wakes the event wait below, so its work is handled at once
    env_.setWakeHook([] { Window::wakeEventLoop(); });
}

void WindowSystem::stop(env::Environment &)
{
    env_.setWakeHook(nullptr);
}

void WindowSystem::tick(env::Environment &env)
{
    if (windows_.empty()) {
        env.requestStop();
        return;
    }

    // Events for every window. With nothing visible no daemon waits on vsync, so sleep until
    // something happens instead of spinning. A visible window sleeps only as long as the UI
    // asked (input and wakes end the sleep at once).
    bool anyVisible = false;
    for (const auto &e : windows_)
        anyVisible = anyVisible || isVisible(e->id);
    if (!anyVisible)
        windows_.front()->window->waitEvents(0.05);
    else if (nextWait_ > 0.0)
        windows_.front()->window->waitEvents(nextWait_);
    else
        windows_.front()->window->pollEvents();
    nextWait_ = 0.0;

    // Closing the main window ends the program; other windows just go
    for (size_t i = windows_.size(); i-- > 0;) {
        Entry &e = *windows_[i];
        if (!e.window->shouldClose() && !e.closeRequested)
            continue;
        if (!e.closeRequested && e.onCloseRequest && !e.onCloseRequest()) {
            e.window->cancelClose();
            continue;
        }
        if (e.id == main_) {
            env.requestStop();
            continue;
        }
        env.events().publish(WindowClosing{e.id});
        windows_.erase(windows_.begin() + static_cast<long>(i));
    }
}

}
