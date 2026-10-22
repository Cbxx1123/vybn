// Windows 10 APIs (SM_CXPADDEDBORDER, DWM) for the custom title bar. Must come before any
// standard header, since MinGW's headers otherwise settle on an older Windows version.
#if defined(_WIN32) && !defined(_WIN32_WINNT)
#define _WIN32_WINNT 0x0A00
#endif

#include "Window.h"

#include <iterator>
#include <stdexcept>

#include <GLFW/glfw3.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>
#include <dwmapi.h>
#include <windows.h>
#include <windowsx.h>
#endif

namespace engine {

namespace {

int openWindows = 0;
// The system's standard cursors, made when first used; GLFW frees them when it terminates
GLFWcursor *standardCursors[8] = {};

#ifdef _WIN32
const wchar_t *kWindowProperty = L"engine::Window";

// Thickness of the invisible resize border around a window that has no system frame
int frameThickness(bool vertical)
{
    return GetSystemMetrics(vertical ? SM_CYFRAME : SM_CXFRAME) + GetSystemMetrics(SM_CXPADDEDBORDER);
}
#endif

}

Window::Window(uint32_t width, uint32_t height, const std::string &title, bool visible, bool decorated)
{
    if (openWindows == 0 && !glfwInit())
        throw std::runtime_error("Failed to initialise GLFW");

    // No OpenGL context; Vulkan owns the surface
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    glfwWindowHint(GLFW_VISIBLE, visible ? GLFW_TRUE : GLFW_FALSE);
    glfwWindowHint(GLFW_DECORATED, decorated ? GLFW_TRUE : GLFW_FALSE);
    window_ = glfwCreateWindow(static_cast<int>(width), static_cast<int>(height), title.c_str(), nullptr, nullptr);
    if (!window_) {
        if (openWindows == 0)
            glfwTerminate();
        throw std::runtime_error("Failed to create GLFW window");
    }
    ++openWindows;

    // Input arrives through callbacks and is queued, in order, until the UI takes it
    glfwSetWindowUserPointer(window_, this);
    glfwSetCharCallback(window_, &Window::onChar);
    glfwSetKeyCallback(window_, &Window::onKey);
    glfwSetMouseButtonCallback(window_, &Window::onMouseButton);
    glfwSetCursorPosCallback(window_, &Window::onCursor);
    glfwSetScrollCallback(window_, &Window::onScroll);
    glfwSetWindowRefreshCallback(window_, &Window::onRefresh);
    glfwSetDropCallback(window_, &Window::onDrop);
}

Window::~Window()
{
    glfwDestroyWindow(window_);
    if (--openWindows == 0) {
        glfwTerminate();
        for (GLFWcursor *&c : standardCursors)
            c = nullptr;
    }
}

void Window::setCursor(Cursor cursor)
{
    if (cursor == cursor_)
        return;
    cursor_ = cursor;
    const size_t index = static_cast<size_t>(cursor);
    if (cursor != Cursor::Arrow && index < std::size(standardCursors) && !standardCursors[index]) {
        int shape = GLFW_ARROW_CURSOR;
        switch (cursor) {
        case Cursor::Hand: shape = GLFW_POINTING_HAND_CURSOR; break;
        case Cursor::Crosshair: shape = GLFW_CROSSHAIR_CURSOR; break;
        case Cursor::IBeam: shape = GLFW_IBEAM_CURSOR; break;
        case Cursor::ResizeX: shape = GLFW_RESIZE_EW_CURSOR; break;
        case Cursor::ResizeY: shape = GLFW_RESIZE_NS_CURSOR; break;
        case Cursor::Move: shape = GLFW_RESIZE_ALL_CURSOR; break;
        case Cursor::NotAllowed: shape = GLFW_NOT_ALLOWED_CURSOR; break;
        case Cursor::Arrow: break;
        }
        standardCursors[index] = glfwCreateStandardCursor(shape);
    }
    // Null (the arrow, or a shape the system lacks) is the default cursor
    glfwSetCursor(window_, cursor == Cursor::Arrow ? nullptr : standardCursors[index]);
}

bool Window::shouldClose() const
{
    return glfwWindowShouldClose(window_);
}

void Window::pollEvents()
{
    glfwPollEvents();
}

void Window::waitEvents()
{
    glfwWaitEvents();
}

void Window::waitEvents(double timeoutSeconds)
{
    glfwWaitEventsTimeout(timeoutSeconds);
}

void Window::wakeEventLoop()
{
    // Thread-safe in GLFW; a wake posted before the wait makes that wait return at once
    glfwPostEmptyEvent();
}

void Window::position(int &x, int &y) const
{
    glfwGetWindowPos(window_, &x, &y);
}

void Window::setPosition(int x, int y)
{
    glfwSetWindowPos(window_, x, y);
}

void Window::focus()
{
    glfwFocusWindow(window_);
}

void Window::setSize(int width, int height)
{
    glfwSetWindowSize(window_, width, height);
}

void Window::show()
{
    glfwShowWindow(window_);
}

void Window::hide()
{
    glfwHideWindow(window_);
}

void Window::centerOnScreen()
{
    GLFWmonitor *monitor = glfwGetPrimaryMonitor();
    const GLFWvidmode *mode = monitor ? glfwGetVideoMode(monitor) : nullptr;
    if (!mode)
        return;
    int mx = 0, my = 0, w = 0, h = 0;
    glfwGetMonitorPos(monitor, &mx, &my);
    glfwGetWindowSize(window_, &w, &h);
    glfwSetWindowPos(window_, mx + (mode->width - w) / 2, my + (mode->height - h) / 2);
}

void Window::setTitle(const std::string &title)
{
    glfwSetWindowTitle(window_, title.c_str());
}

void Window::setOpacity(float opacity)
{
    glfwSetWindowOpacity(window_, opacity < 0.0f ? 0.0f : opacity > 1.0f ? 1.0f : opacity);
}

bool Window::desktopCursor(int &x, int &y)
{
#ifdef _WIN32
    POINT p;
    if (!GetCursorPos(&p))
        return false;
    x = p.x;
    y = p.y;
    return true;
#else
    (void)x;
    (void)y;
    return false;
#endif
}

bool Window::primaryButtonHeld()
{
#ifdef _WIN32
    // With the buttons swapped, the primary one is the right
    const int button = GetSystemMetrics(SM_SWAPBUTTON) ? VK_RBUTTON : VK_LBUTTON;
    return (GetAsyncKeyState(button) & 0x8000) != 0;
#else
    return false;
#endif
}

void Window::framebufferSize(int &width, int &height) const
{
    glfwGetFramebufferSize(window_, &width, &height);
}

void Window::size(int &width, int &height) const
{
    glfwGetWindowSize(window_, &width, &height);
}

void Window::cursorPosition(double &x, double &y) const
{
    glfwGetCursorPos(window_, &x, &y);
}

bool Window::mouseButtonDown(int button) const
{
    return glfwGetMouseButton(window_, button) == GLFW_PRESS;
}

std::vector<InputEvent> Window::takeEvents()
{
    std::vector<InputEvent> events;
    events.swap(events_);
    return events;
}

uint8_t Window::modifiers() const
{
    auto held = [this](int a, int b) {
        return glfwGetKey(window_, a) == GLFW_PRESS || glfwGetKey(window_, b) == GLFW_PRESS;
    };
    uint8_t mods = 0;
    if (held(GLFW_KEY_LEFT_SHIFT, GLFW_KEY_RIGHT_SHIFT))
        mods |= ModShift;
    if (held(GLFW_KEY_LEFT_CONTROL, GLFW_KEY_RIGHT_CONTROL))
        mods |= ModCtrl;
    if (held(GLFW_KEY_LEFT_ALT, GLFW_KEY_RIGHT_ALT))
        mods |= ModAlt;
    if (held(GLFW_KEY_LEFT_SUPER, GLFW_KEY_RIGHT_SUPER))
        mods |= ModSuper;
    return mods;
}

std::string Window::clipboardText() const
{
    const char *text = glfwGetClipboardString(window_);
    return text ? text : "";
}

void Window::setClipboardText(const std::string &text)
{
    glfwSetClipboardString(window_, text.c_str());
}

double Window::time() const
{
    return glfwGetTime();
}

void *Window::nativeHandle() const
{
#ifdef _WIN32
    return glfwGetWin32Window(window_);
#else
    return nullptr;
#endif
}

bool Window::useCustomTitleBar(std::function<bool(double x, double y)> isCaption)
{
    isCaption_ = std::move(isCaption);
#ifdef _WIN32
    if (customTitleBar_)
        return true;
    HWND hwnd = glfwGetWin32Window(window_);

    // Hook in front of GLFW's window procedure; Window::frameProc forwards everything it
    // doesn't handle
    SetPropW(hwnd, kWindowProperty, this);
    previousProc_ = reinterpret_cast<void *>(GetWindowLongPtrW(hwnd, GWLP_WNDPROC));
    SetWindowLongPtrW(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&Window::frameProc));

    // A one-pixel sliver of DWM frame keeps the system shadow; the app paints over it
    const MARGINS margins{0, 0, 1, 0};
    DwmExtendFrameIntoClientArea(hwnd, &margins);

    // Recalculate the frame, which now has no title bar (see WM_NCCALCSIZE in frameProc)
    SetWindowPos(hwnd, nullptr, 0, 0, 0, 0, SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    customTitleBar_ = true;
    return true;
#else
    return false;
#endif
}

#ifdef _WIN32
intptr_t __stdcall Window::frameProc(void *handle, unsigned int message, uintptr_t wParam, intptr_t lParam)
{
    HWND hwnd = static_cast<HWND>(handle);
    auto *self = static_cast<Window *>(GetPropW(hwnd, kWindowProperty));
    auto previous = reinterpret_cast<WNDPROC>(self->previousProc_);

    switch (message) {
    case WM_NCCALCSIZE:
        // The client area takes the whole window. A maximised window hangs its frame off the
        // screen edges, so pull the client area back onto the screen.
        if (wParam == TRUE) {
            if (IsZoomed(hwnd)) {
                auto *params = reinterpret_cast<NCCALCSIZE_PARAMS *>(lParam);
                const int x = frameThickness(false);
                const int y = frameThickness(true);
                params->rgrc[0].left += x;
                params->rgrc[0].right -= x;
                params->rgrc[0].top += y;
                params->rgrc[0].bottom -= y;
            }
            return 0;
        }
        break;
    case WM_NCHITTEST: {
        POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        RECT window{};
        GetWindowRect(hwnd, &window);

        // Edges and corners resize, unless maximised
        if (!IsZoomed(hwnd)) {
            const int bx = frameThickness(false);
            const int by = frameThickness(true);
            const bool left = point.x < window.left + bx;
            const bool right = point.x >= window.right - bx;
            const bool top = point.y < window.top + by;
            const bool bottom = point.y >= window.bottom - by;
            if (top && left)
                return HTTOPLEFT;
            if (top && right)
                return HTTOPRIGHT;
            if (bottom && left)
                return HTBOTTOMLEFT;
            if (bottom && right)
                return HTBOTTOMRIGHT;
            if (top)
                return HTTOP;
            if (bottom)
                return HTBOTTOM;
            if (left)
                return HTLEFT;
            if (right)
                return HTRIGHT;
        }

        ScreenToClient(hwnd, &point);
        if (self->isCaption_ && self->isCaption_(point.x, point.y))
            return HTCAPTION;
        return HTCLIENT;
    }
    case WM_NCDESTROY:
        // Unhook before the window goes away
        SetWindowLongPtrW(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(previous));
        RemovePropW(hwnd, kWindowProperty);
        break;
    default:
        break;
    }
    return CallWindowProcW(previous, hwnd, message, wParam, lParam);
}
#endif

void Window::minimize()
{
    glfwIconifyWindow(window_);
}

void Window::toggleMaximize()
{
    if (isMaximized())
        glfwRestoreWindow(window_);
    else
        glfwMaximizeWindow(window_);
}

bool Window::isMaximized() const
{
    return glfwGetWindowAttrib(window_, GLFW_MAXIMIZED) == GLFW_TRUE;
}

void Window::requestClose()
{
    glfwSetWindowShouldClose(window_, GLFW_TRUE);
}

void Window::cancelClose()
{
    glfwSetWindowShouldClose(window_, GLFW_FALSE);
}

namespace {

uint8_t modsFrom(int glfwMods)
{
    uint8_t mods = 0;
    if (glfwMods & GLFW_MOD_SHIFT)
        mods |= ModShift;
    if (glfwMods & GLFW_MOD_CONTROL)
        mods |= ModCtrl;
    if (glfwMods & GLFW_MOD_ALT)
        mods |= ModAlt;
    if (glfwMods & GLFW_MOD_SUPER)
        mods |= ModSuper;
    return mods;
}

Key keyFrom(int key)
{
    if (key >= GLFW_KEY_A && key <= GLFW_KEY_Z)
        return static_cast<Key>(static_cast<int>(Key::A) + (key - GLFW_KEY_A));
    if (key >= GLFW_KEY_0 && key <= GLFW_KEY_9)
        return static_cast<Key>(static_cast<int>(Key::Num0) + (key - GLFW_KEY_0));
    if (key >= GLFW_KEY_F1 && key <= GLFW_KEY_F12)
        return static_cast<Key>(static_cast<int>(Key::F1) + (key - GLFW_KEY_F1));
    if (key >= GLFW_KEY_KP_0 && key <= GLFW_KEY_KP_9)
        return static_cast<Key>(static_cast<int>(Key::Keypad0) + (key - GLFW_KEY_KP_0));
    switch (key) {
    case GLFW_KEY_SPACE: return Key::Space;
    case GLFW_KEY_ENTER:
    case GLFW_KEY_KP_ENTER: return Key::Enter;
    case GLFW_KEY_ESCAPE: return Key::Escape;
    case GLFW_KEY_TAB: return Key::Tab;
    case GLFW_KEY_BACKSPACE: return Key::Backspace;
    case GLFW_KEY_DELETE: return Key::Delete;
    case GLFW_KEY_INSERT: return Key::Insert;
    case GLFW_KEY_LEFT: return Key::Left;
    case GLFW_KEY_RIGHT: return Key::Right;
    case GLFW_KEY_UP: return Key::Up;
    case GLFW_KEY_DOWN: return Key::Down;
    case GLFW_KEY_HOME: return Key::Home;
    case GLFW_KEY_END: return Key::End;
    case GLFW_KEY_PAGE_UP: return Key::PageUp;
    case GLFW_KEY_PAGE_DOWN: return Key::PageDown;
    case GLFW_KEY_MINUS: return Key::Minus;
    case GLFW_KEY_EQUAL: return Key::Equal;
    case GLFW_KEY_LEFT_BRACKET: return Key::LeftBracket;
    case GLFW_KEY_RIGHT_BRACKET: return Key::RightBracket;
    case GLFW_KEY_BACKSLASH: return Key::Backslash;
    case GLFW_KEY_SEMICOLON: return Key::Semicolon;
    case GLFW_KEY_APOSTROPHE: return Key::Apostrophe;
    case GLFW_KEY_COMMA: return Key::Comma;
    case GLFW_KEY_PERIOD: return Key::Period;
    case GLFW_KEY_SLASH: return Key::Slash;
    case GLFW_KEY_GRAVE_ACCENT: return Key::Grave;
    case GLFW_KEY_KP_DECIMAL: return Key::KeypadDecimal;
    case GLFW_KEY_KP_DIVIDE: return Key::KeypadDivide;
    case GLFW_KEY_KP_MULTIPLY: return Key::KeypadMultiply;
    case GLFW_KEY_KP_SUBTRACT: return Key::KeypadSubtract;
    case GLFW_KEY_KP_ADD: return Key::KeypadAdd;
    case GLFW_KEY_LEFT_SHIFT:
    case GLFW_KEY_RIGHT_SHIFT: return Key::Shift;
    case GLFW_KEY_LEFT_CONTROL:
    case GLFW_KEY_RIGHT_CONTROL: return Key::Ctrl;
    case GLFW_KEY_LEFT_ALT:
    case GLFW_KEY_RIGHT_ALT: return Key::Alt;
    case GLFW_KEY_LEFT_SUPER:
    case GLFW_KEY_RIGHT_SUPER: return Key::Super;
    case GLFW_KEY_CAPS_LOCK: return Key::CapsLock;
    case GLFW_KEY_PRINT_SCREEN: return Key::PrintScreen;
    case GLFW_KEY_PAUSE: return Key::Pause;
    case GLFW_KEY_MENU: return Key::Menu;
    default: return Key::Unknown;
    }
}

Window &self(GLFWwindow *window)
{
    return *static_cast<Window *>(glfwGetWindowUserPointer(window));
}

}

void Window::onKey(GLFWwindow *window, int key, int, int action, int mods)
{
    KeyEvent e;
    e.key = keyFrom(key);
    e.mods = modsFrom(mods);
    e.down = action != GLFW_RELEASE;
    e.repeat = action == GLFW_REPEAT;
    // A modifier key's own press reports its bit as already held; its release, not yet released
    if (isModifier(e.key)) {
        const uint8_t bit = e.key == Key::Shift ? ModShift : e.key == Key::Ctrl ? ModCtrl : e.key == Key::Alt ? ModAlt : ModSuper;
        e.mods = e.down ? (e.mods | bit) : (e.mods & ~bit);
    }
    self(window).events_.push_back(e);
}

void Window::onChar(GLFWwindow *window, unsigned int codepoint)
{
    TextEvent e;
    e.codepoint = codepoint;
    self(window).events_.push_back(e);
}

void Window::onMouseButton(GLFWwindow *window, int button, int action, int mods)
{
    MouseButtonEvent e;
    switch (button) {
    case GLFW_MOUSE_BUTTON_LEFT: e.button = MouseButton::Left; break;
    case GLFW_MOUSE_BUTTON_RIGHT: e.button = MouseButton::Right; break;
    case GLFW_MOUSE_BUTTON_MIDDLE: e.button = MouseButton::Middle; break;
    case GLFW_MOUSE_BUTTON_4: e.button = MouseButton::Back; break;
    case GLFW_MOUSE_BUTTON_5: e.button = MouseButton::Forward; break;
    default: return;
    }
    e.down = action == GLFW_PRESS;
    e.mods = modsFrom(mods);
    glfwGetCursorPos(window, &e.x, &e.y);
    self(window).events_.push_back(e);
}

void Window::onCursor(GLFWwindow *window, double x, double y)
{
    auto &events = self(window).events_;
    // Only the latest position matters between other events
    if (!events.empty()) {
        if (auto *last = std::get_if<MouseMoveEvent>(&events.back())) {
            *last = {x, y};
            return;
        }
    }
    events.push_back(MouseMoveEvent{x, y});
}

void Window::onScroll(GLFWwindow *window, double x, double y)
{
    ScrollEvent e;
    e.x = x;
    e.y = y;
    e.mods = self(window).modifiers();
    self(window).events_.push_back(e);
}

void Window::onDrop(GLFWwindow *window, int count, const char **paths)
{
    Drop drop;
    for (int i = 0; i < count; ++i) {
        if (paths[i])
            drop.paths.emplace_back(paths[i]);
    }
    // GLFW moves its cursor to the drop's place before calling this
    glfwGetCursorPos(window, &drop.x, &drop.y);
    if (!drop.paths.empty())
        self(window).drops_.push_back(std::move(drop));
}

std::vector<Window::Drop> Window::takeDrops()
{
    std::vector<Drop> drops = std::move(drops_);
    drops_.clear();
    return drops;
}

void Window::onRefresh(GLFWwindow *window)
{
    self(window).refreshRequested_ = true;
}

bool Window::takeRefreshRequested()
{
    const bool requested = refreshRequested_;
    refreshRequested_ = false;
    return requested;
}

}
