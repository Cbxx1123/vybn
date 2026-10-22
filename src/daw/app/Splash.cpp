// Windows 10 for the DPI functions, set before any include (MinGW's headers default to XP)
#if defined(_WIN32) && !defined(_WIN32_WINNT)
#define _WIN32_WINNT 0x0A00
#endif

#include "daw/app/Splash.h"

#include "daw/app/AppIcon.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <string>

#ifdef _WIN32
#include <windows.h>
#include <dwmapi.h>
#include <gdiplus.h>
#endif

namespace daw {

#ifdef _WIN32

namespace {

constexpr int kCardWidth = 320;
constexpr int kCardHeight = 280;
constexpr int kMargin = 28;      // room round the card for its shadow
constexpr double kPi = 3.14159265358979323846;
constexpr double kLap = 1.15;       // seconds a swoop round the logo takes
constexpr int kTail = 26;           // dots in the trail behind the head
constexpr double kTailStep = 0.009; // seconds between them
constexpr double kFade = 0.16;      // seconds to fade out when done

// Slow at the top, fast round the bottom: one big swoop a lap
double swoop(double p)
{
    return p < 0.5 ? 4.0 * p * p * p : 1.0 - std::pow(-2.0 * p + 2.0, 3.0) / 2.0;
}

// The dot's angle at `t` seconds, starting at the top and going clockwise
double angleAt(double t)
{
    const double laps = t / kLap;
    return -kPi / 2.0 + 2.0 * kPi * (std::floor(laps) + swoop(laps - std::floor(laps)));
}

ui::Color mix(ui::Color a, ui::Color b, float t)
{
    return {a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t, 1.0f};
}

Gdiplus::Color gdi(ui::Color c, float alpha = 1.0f)
{
    auto byte = [](float v) { return static_cast<BYTE>(std::lround((std::min)(1.0f, (std::max)(0.0f, v)) * 255.0f)); };
    return Gdiplus::Color(byte(alpha), byte(c.r), byte(c.g), byte(c.b));
}

void roundedRect(Gdiplus::GraphicsPath &path, float x, float y, float w, float h, float r)
{
    const float d = 2.0f * r;
    path.AddArc(x, y, d, d, 180.0f, 90.0f);
    path.AddArc(x + w - d, y, d, d, 270.0f, 90.0f);
    path.AddArc(x + w - d, y + h - d, d, d, 0.0f, 90.0f);
    path.AddArc(x, y + h - d, d, d, 90.0f, 90.0f);
    path.CloseFigure();
}

void fillCircle(Gdiplus::Graphics &g, const Gdiplus::Brush &brush, float cx, float cy, float r)
{
    g.FillEllipse(&brush, cx - r, cy - r, 2.0f * r, 2.0f * r);
}

LRESULT CALLBACK splashProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

std::wstring widen(const std::string &text)
{
    const int n = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0);
    std::wstring out(static_cast<size_t>((std::max)(0, n - 1)), L'\0');
    if (n > 1)
        MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, out.data(), n);
    return out;
}

float systemScale()
{
    using GetDpi = UINT(WINAPI *)();
    if (auto get = reinterpret_cast<GetDpi>(reinterpret_cast<void *>(GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForSystem"))))
        return static_cast<float>(get()) / 96.0f;
    return 1.0f;
}

}

Splash::Splash(const ui::Theme &theme)
    : base_(theme.baseColor()), accent_(theme.accentColor()), text_(theme.textColor(ui::TextTone::Primary)),
      muted_(theme.textColor(ui::TextTone::Muted))
{
    // Sharp on high-DPI screens: the same awareness GLFW asks for later
    using SetAwareness = BOOL(WINAPI *)(HANDLE);
    if (auto set = reinterpret_cast<SetAwareness>(
            reinterpret_cast<void *>(GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetProcessDpiAwarenessContext"))))
        set(reinterpret_cast<HANDLE>(static_cast<intptr_t>(-4))); // per monitor, version 2
    thread_ = std::thread([this] { run(); });
}

Splash::~Splash()
{
    close();
    if (thread_.joinable())
        thread_.join();
}

void Splash::run()
{
    Gdiplus::GdiplusStartupInput startup;
    ULONG_PTR token = 0;
    if (Gdiplus::GdiplusStartup(&token, &startup, nullptr) != Gdiplus::Ok)
        return;
    {
        const float scale = systemScale();
        const int width = static_cast<int>((kCardWidth + 2 * kMargin) * scale);
        const int height = static_cast<int>((kCardHeight + 2 * kMargin) * scale);

        // The window: borderless, per-pixel alpha, no taskbar button, in the middle of the screen
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = splashProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursorW(nullptr, reinterpret_cast<LPCWSTR>(IDC_APPSTARTING));
        wc.lpszClassName = L"VibinSplash";
        RegisterClassExW(&wc);
        RECT work{};
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
        const POINT position{work.left + (work.right - work.left - width) / 2, work.top + (work.bottom - work.top - height) / 2};
        HWND hwnd = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOOLWINDOW, wc.lpszClassName, L"Vibin", WS_POPUP, position.x,
                                    position.y, width, height, nullptr, nullptr, wc.hInstance, nullptr);

        // A premultiplied 32-bit bitmap to draw into and hand to the compositor
        HDC screen = GetDC(nullptr);
        HDC memory = CreateCompatibleDC(screen);
        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = width;
        info.bmiHeader.biHeight = -height; // top down
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        void *bits = nullptr;
        HBITMAP dib = CreateDIBSection(screen, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
        HGDIOBJ previous = SelectObject(memory, dib);

        // The app's font for the name, else the system's
        Gdiplus::PrivateFontCollection fonts;
        fonts.AddFontFile(widen(std::string(ENGINE_ASSET_DIR) + "/fonts/Inter-Medium.ttf").c_str());
        Gdiplus::FontFamily family;
        int found = 0;
        if (fonts.GetFamilyCount() > 0)
            fonts.GetFamilies(1, &family, &found);
        Gdiplus::FontFamily fallback(L"Segoe UI");
        const Gdiplus::FontFamily &face = found > 0 ? family : fallback;
        Gdiplus::Font title(&face, 22.0f * scale, Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
        Gdiplus::Font caption(&face, 12.0f * scale, Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
        Gdiplus::StringFormat centred;
        centred.SetAlignment(Gdiplus::StringAlignmentCenter);
        centred.SetLineAlignment(Gdiplus::StringAlignmentCenter);

        const ui::Color shadow = mix(base_, {0.0f, 0.0f, 0.0f, 1.0f}, 0.55f);
        const ui::Color light = mix(base_, {1.0f, 1.0f, 1.0f, 1.0f}, 0.10f);

        const auto start = std::chrono::steady_clock::now();
        double stoppedAt = -1.0;
        bool shown = false;
        while (true) {
            MSG message;
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
            const double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
            if (stop_.load(std::memory_order_acquire) && stoppedAt < 0.0)
                stoppedAt = t;
            const float fade = stoppedAt < 0.0 ? 1.0f : static_cast<float>(1.0 - (t - stoppedAt) / kFade);
            if (fade <= 0.0f)
                break;

            {
                Gdiplus::Bitmap canvas(width, height, width * 4, PixelFormat32bppPARGB, static_cast<BYTE *>(bits));
                Gdiplus::Graphics g(&canvas);
                g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
                g.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAliasGridFit);
                g.Clear(Gdiplus::Color(0, 0, 0, 0));
                g.ScaleTransform(scale, scale);

                // The card, on a soft shadow
                const float cx = kMargin, cy = kMargin, cw = kCardWidth, ch = kCardHeight;
                for (int i = 14; i >= 1; --i) {
                    Gdiplus::GraphicsPath path;
                    roundedRect(path, cx - i, cy - i + 6.0f, cw + 2.0f * i, ch + 2.0f * i, 22.0f + i);
                    const float f = static_cast<float>(i) / 14.0f;
                    Gdiplus::SolidBrush brush(gdi(shadow, 0.10f * (1.0f - f) * (1.0f - f)));
                    g.FillPath(&brush, &path);
                }
                Gdiplus::GraphicsPath card;
                roundedRect(card, cx, cy, cw, ch, 22.0f);
                Gdiplus::SolidBrush baseBrush(gdi(base_));
                g.FillPath(&baseBrush, &card);
                Gdiplus::Pen rim(gdi(light, 0.6f), 1.0f);
                g.DrawPath(&rim, &card);

                // The groove the dot runs in: pressed into the card, dark at the top left and lit
                // at the bottom right
                const float ox = cx + cw * 0.5f, oy = cy + 112.0f;
                const float groove = 70.0f;
                Gdiplus::SolidBrush deep(gdi(mix(base_, shadow, 0.18f)));
                fillCircle(g, deep, ox, oy, groove);
                {
                    Gdiplus::Pen dark(gdi(shadow, 0.55f), 3.0f);
                    g.DrawArc(&dark, ox - groove + 1.5f, oy - groove + 1.5f, 2.0f * groove - 3.0f, 2.0f * groove - 3.0f, 135.0f, 180.0f);
                    Gdiplus::Pen lit(gdi(light, 0.7f), 2.0f);
                    g.DrawArc(&lit, ox - groove + 1.0f, oy - groove + 1.0f, 2.0f * groove - 2.0f, 2.0f * groove - 2.0f, 315.0f, 180.0f);
                }

                // The logo: a raised, slightly domed disc with the waveform (the same mark as the app's icon)
                drawLogoMark(g, ox, oy, 42.0f, base_, accent_);

                // The dot and its trail: where it was a moment ago, so the trail stretches as it
                // speeds round the bottom and gathers as it slows over the top
                const float orbit = 58.0f, head = 5.2f;
                for (int k = kTail; k >= 0; --k) {
                    const double a = angleAt(t - k * kTailStep);
                    const float f = 1.0f - static_cast<float>(k) / (kTail + 1);
                    Gdiplus::SolidBrush dot(gdi(accent_, k == 0 ? 1.0f : 0.8f * f * f));
                    fillCircle(g, dot, ox + static_cast<float>(std::cos(a)) * orbit, oy + static_cast<float>(std::sin(a)) * orbit,
                               head * (0.35f + 0.65f * f));
                }

                Gdiplus::SolidBrush name(gdi(text_));
                Gdiplus::SolidBrush note(gdi(muted_));
                g.DrawString(L"Vibin", -1, &title, Gdiplus::RectF(cx, oy + groove + 14.0f, cw, 32.0f), &centred, &name);
                g.DrawString(L"Loading", -1, &caption, Gdiplus::RectF(cx, oy + groove + 44.0f, cw, 18.0f), &centred, &note);
            }

            SIZE size{width, height};
            POINT origin{0, 0};
            BLENDFUNCTION blend{AC_SRC_OVER, 0, static_cast<BYTE>(std::lround(255.0f * (std::min)(1.0f, fade))), AC_SRC_ALPHA};
            POINT at = position;
            UpdateLayeredWindow(hwnd, screen, &at, &size, memory, &origin, 0, &blend, ULW_ALPHA);
            if (!shown) {
                ShowWindow(hwnd, SW_SHOWNORMAL);
                shown = true;
            }
            // In step with the display; a sleep if the compositor is off
            if (FAILED(DwmFlush()))
                Sleep(15);
        }

        DestroyWindow(hwnd);
        SelectObject(memory, previous);
        DeleteObject(dib);
        DeleteDC(memory);
        ReleaseDC(nullptr, screen);
        UnregisterClassW(wc.lpszClassName, wc.hInstance);
    }
    Gdiplus::GdiplusShutdown(token);
}

#else

Splash::Splash(const ui::Theme &) {}
Splash::~Splash() = default;
void Splash::run() {}

#endif

}
