#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "InputEvents.h"

struct GLFWwindow;

namespace engine {

// Opens a window on construction and closes it on destruction.
// GLFW is initialised by the first Window and terminated after the last one closes.
class Window {
public:
    // A hidden window (`visible` false) shows once show() is called
    // `decorated` false leaves out the system's border and title bar (a splash screen)
    Window(uint32_t width, uint32_t height, const std::string &title, bool visible = true, bool decorated = true);
    ~Window();

    Window(const Window &) = delete;
    Window &operator=(const Window &) = delete;

    bool shouldClose() const;
    void pollEvents();
    void waitEvents();
    void waitEvents(double timeoutSeconds);
    void framebufferSize(int &width, int &height) const;
    void size(int &width, int &height) const;

    // Cursor position in window coordinates (may differ from framebuffer pixels on high-DPI displays)
    void cursorPosition(double &x, double &y) const;
    bool mouseButtonDown(int button = 0) const;

    // Everything the user did since the last call, in order: keys (presses, repeats and
    // releases, with the modifiers held), typed characters, mouse buttons, cursor moves and the
    // wheel. Nothing is lost between frames, however quick.
    std::vector<InputEvent> takeEvents();
    // Whether any input is waiting to be taken
    bool hasEvents() const { return !events_.empty(); }
    // Wakes a waitEvents call on any thread (a daemon posted something for the main thread)
    static void wakeEventLoop();
    // The modifiers held now (Mod bits)
    uint8_t modifiers() const;
    std::string clipboardText() const;
    void setClipboardText(const std::string &text);

    // Seconds since GLFW was initialised
    double time() const;

    // Position of the window's top-left corner on the desktop, in screen coordinates
    void position(int &x, int &y) const;
    void setPosition(int x, int y);
    // Size in screen coordinates (the swapchain follows at the next frame)
    void setSize(int width, int height);
    void focus();
    // The system's handle for it (an HWND on Windows; null elsewhere)
    void *nativeHandle() const;
    void show();
    void setTitle(const std::string &title);
    // Moves it to the middle of the primary monitor
    void centerOnScreen();
    void hide();
    // How see-through the whole window is: 1 opaque, 0 invisible
    void setOpacity(float opacity);
    // The mouse pointer's shape while it is over the window (only changed when it differs)
    void setCursor(Cursor cursor);

    // Where the mouse is on the desktop (screen coordinates), and whether its primary button is
    // held, wherever the mouse is and whichever window has it. Windows only: elsewhere they
    // return false.
    static bool desktopCursor(int &x, int &y);
    static bool primaryButtonHeld();

    // Replaces the system title bar with one the app draws. The client area grows to cover
    // the whole window, and `isCaption` says which client points (window coordinates) drag the
    // window like a title bar. Double-clicking them maximises, the edges still resize, and the
    // system shadow and snapping stay. Windows only: elsewhere it does nothing and returns false.
    bool useCustomTitleBar(std::function<bool(double x, double y)> isCaption);
    bool hasCustomTitleBar() const { return customTitleBar_; }

    // Window buttons, for a custom title bar
    void minimize();
    void toggleMaximize();
    bool isMaximized() const;
    void requestClose();
    // Takes back a close the user asked for (the close button, Alt+F4), before it happens
    void cancelClose();
    // Whether the system asked for the window to be redrawn since the last call (it was exposed,
    // restored or uncovered), and clears the request
    bool takeRefreshRequested();

    GLFWwindow *handle() const { return window_; }

private:
    static void onChar(GLFWwindow *window, unsigned int codepoint);
    static void onKey(GLFWwindow *window, int key, int scancode, int action, int mods);
    static void onMouseButton(GLFWwindow *window, int button, int action, int mods);
    static void onCursor(GLFWwindow *window, double x, double y);
    static void onScroll(GLFWwindow *window, double x, double y);
    static void onRefresh(GLFWwindow *window);
#ifdef _WIN32
    // Window procedure for the custom title bar (a WNDPROC, spelled without windows.h)
    static intptr_t __stdcall frameProc(void *hwnd, unsigned int message, uintptr_t wParam, intptr_t lParam);
#endif

    GLFWwindow *window_ = nullptr;
    std::vector<InputEvent> events_;
    bool customTitleBar_ = false;
    bool refreshRequested_ = false;
    Cursor cursor_ = Cursor::Arrow;
    std::function<bool(double, double)> isCaption_;
    void *previousProc_ = nullptr; // the window procedure the custom title bar hooks in front of
};

}
