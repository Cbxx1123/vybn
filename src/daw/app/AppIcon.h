#pragma once

#include <string>

#include "ui/Theme.h"

#ifdef _WIN32
namespace Gdiplus {
class Graphics;
}
#endif

namespace daw {

// The app's icon: the loading screen's card, without its name and its swooping dot's trail: a rounded
// tile in the theme's base colour, the groove the dot runs in, the raised disc with its waveform in the
// accent colour, and the dot. Drawn from the theme's two colours, so it follows the look: the window's
// icon (the taskbar, Alt-Tab) is made from the current theme and replaced when the theme changes, and
// the executable's own icon (assets/icon/vybn.ico, embedded by src/win/vybn.rc) is the default look.
// Windows only (GDI+); elsewhere these do nothing and report failure.

// Sets the window's big and small icons from the theme's colours. `window` is Window::nativeHandle()
// (an HWND). Returns false when it couldn't (not Windows, no window, GDI+ failing).
bool setWindowIcon(void *window, ui::Color base, ui::Color accent);

// The executable's own icon is data in the file, which can't change while the program runs. So when the
// app ends with colours whose icon differs from the one inside the executable, this starts
// vybn-icon-patch.exe (beside the program), which waits for the app to be gone and then gives the
// executable the new icon (src/win/IconPatch.cpp: its images overwritten in place, byte for byte).
// Explorer and a pinned taskbar button show it from then on. Call it as the app ends. Does nothing when
// the executable already has this icon, or the patcher isn't there; returns whether it was started.
bool scheduleExecutableIcon(ui::Color base, ui::Color accent);

// The app is a console program, so started from Explorer (or anywhere that gives it a console of its
// own) it has a console window, and Windows groups that with the app's window in one taskbar button
// that shows the executable's icon rather than the window's. This takes the console off the taskbar
// (it stays open, for the program's messages) when the program is the only one using it; a console
// shared with a terminal or a shell is left alone. Call it once, early.
void keepConsoleOffTaskbar();

// Writes a Windows .ico of the icon in the given colours (16 to 256 pixels, as plain bitmaps), for the
// executable's resource: `main --write-icon assets/icon/vybn.ico`. Returns false with a reason in `error`.
bool writeIconFile(const std::string &path, ui::Color base, ui::Color accent, std::string &error);

#ifdef _WIN32
// The mark: a raised, slightly domed disc of radius `r` with a waveform of five bars in the accent
// colour, and its soft shadow and highlight, centred on (cx, cy) in the graphics' current transform.
// The loading screen draws its logo with this (r = 42), so the two are the same picture. With `small`
// the waveform is three thicker bars, for icons of a few pixels.
void drawLogoMark(Gdiplus::Graphics &g, float cx, float cy, float r, ui::Color base, ui::Color accent, bool small = false);
#endif

}
