#pragma once

#include <atomic>
#include <thread>

#include "ui/Theme.h"

namespace daw {

// The loading screen: a small card in the middle of the screen with the app's logo (a raised disc
// with a waveform), its name, and a dot that swoops round the logo in a groove, fast round the
// bottom and slow over the top, its trail stretching and gathering with its speed. It shows the
// moment the app starts, so there is no blank window while the app loads, and fades out once the
// main window is on screen (close; the destructor waits for that).
//
// It is a plain Windows layered window, drawn with GDI+ on a thread of its own with its own
// message loop and paced by the compositor (DwmFlush), so it appears at once (no Vulkan device to
// make first) and keeps moving smoothly however busy the main thread is. Elsewhere than Windows it
// does nothing.
class Splash {
public:
    explicit Splash(const ui::Theme &theme);
    ~Splash();

    Splash(const Splash &) = delete;
    Splash &operator=(const Splash &) = delete;

    // Starts fading it out (about a sixth of a second) and returns at once
    void close() { stop_.store(true, std::memory_order_release); }

private:
    void run();

    ui::Color base_, accent_, text_, muted_;
    std::thread thread_;
    std::atomic<bool> stop_{false};
};

}
