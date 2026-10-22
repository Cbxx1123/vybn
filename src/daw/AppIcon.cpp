// Windows 10 for the DPI-aware metrics, set before any include (MinGW's headers default to XP)
#if defined(_WIN32) && !defined(_WIN32_WINNT)
#define _WIN32_WINNT 0x0A00
#endif

#include "AppIcon.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <gdiplus.h>
#endif

namespace daw {

#ifdef _WIN32

namespace {

constexpr double kPi = 3.14159265358979323846;

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

// A soft shadow: rings of falling alpha round a circle
void circleShadow(Gdiplus::Graphics &g, ui::Color colour, float cx, float cy, float r, float spread, float strength)
{
    const int steps = 10;
    for (int i = steps; i >= 1; --i) {
        const float f = static_cast<float>(i) / steps;
        Gdiplus::SolidBrush brush(gdi(colour, strength * (1.0f - f) * (1.0f - f) / 3.0f));
        fillCircle(g, brush, cx, cy, r + spread * f);
    }
}

// GDI+ started for as long as one of these lives (each start has its own token, so the loading
// screen's thread and this don't disturb each other)
struct GdiplusSession {
    ULONG_PTR token = 0;
    bool ok = false;
    GdiplusSession()
    {
        Gdiplus::GdiplusStartupInput startup;
        ok = Gdiplus::GdiplusStartup(&token, &startup, nullptr) == Gdiplus::Ok;
    }
    ~GdiplusSession()
    {
        if (ok)
            Gdiplus::GdiplusShutdown(token);
    }
};

// The icon at `size` pixels square, with its own alpha
std::unique_ptr<Gdiplus::Bitmap> renderIcon(int size, ui::Color base, ui::Color accent)
{
    auto bitmap = std::make_unique<Gdiplus::Bitmap>(size, size, PixelFormat32bppARGB);
    {
        Gdiplus::Graphics g(bitmap.get());
        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        g.Clear(Gdiplus::Color(0, 0, 0, 0));
        const float k = static_cast<float>(size) / 256.0f;
        g.ScaleTransform(k, k); // drawn on a 256 grid
        const bool small = size < 40;

        const ui::Color black{0.0f, 0.0f, 0.0f, 1.0f}, white{1.0f, 1.0f, 1.0f, 1.0f};
        const ui::Color shadow = mix(base, black, 0.55f);
        const ui::Color light = mix(base, white, 0.10f);

        // The tile: the loading screen's card, lit from the top left
        const float margin = 6.0f, side = 244.0f;
        Gdiplus::GraphicsPath tile;
        roundedRect(tile, margin, margin, side, side, 56.0f);
        Gdiplus::LinearGradientBrush face(Gdiplus::PointF(margin, margin), Gdiplus::PointF(margin + side, margin + side),
                                          gdi(mix(base, white, small ? 0.16f : 0.07f)), gdi(mix(base, black, small ? 0.06f : 0.16f)));
        g.FillPath(&face, &tile);
        Gdiplus::Pen rim(gdi(light, 0.7f), small ? 6.0f : 2.5f);
        g.DrawPath(&rim, &tile);

        // The groove the dot runs in: pressed in, dark at the top left and lit at the bottom right
        const float cx = 128.0f, cy = 128.0f, groove = 86.0f;
        if (!small) {
            Gdiplus::SolidBrush deep(gdi(mix(base, shadow, 0.22f)));
            fillCircle(g, deep, cx, cy, groove);
            Gdiplus::Pen dark(gdi(shadow, 0.6f), 4.5f);
            g.DrawArc(&dark, cx - groove + 2.25f, cy - groove + 2.25f, 2.0f * groove - 4.5f, 2.0f * groove - 4.5f, 135.0f, 180.0f);
            Gdiplus::Pen lit(gdi(light, 0.8f), 3.0f);
            g.DrawArc(&lit, cx - groove + 1.5f, cy - groove + 1.5f, 2.0f * groove - 3.0f, 2.0f * groove - 3.0f, 315.0f, 180.0f);
        }

        // The logo, the same mark as the loading screen's
        const float logo = small ? 80.0f : 52.0f; // a small icon is mostly the disc
        drawLogoMark(g, cx, cy, logo, base, accent, small);

        // The dot, with a short trail behind it, at the top right of its orbit
        const float orbit = logo * 58.0f / 42.0f;
        const double head = -kPi / 4.0;
        if (!small) { // too small to show at a few pixels
            const int tail = 12;
            for (int i = tail; i >= 0; --i) {
                const double a = head - i * 0.085;
                const float f = 1.0f - static_cast<float>(i) / (tail + 1);
                Gdiplus::SolidBrush dot(gdi(accent, i == 0 ? 1.0f : 0.8f * f * f));
                fillCircle(g, dot, cx + static_cast<float>(std::cos(a)) * orbit, cy + static_cast<float>(std::sin(a)) * orbit,
                           6.6f * (0.35f + 0.65f * f));
            }
        }
    }
    return bitmap;
}

uint16_t le16(const uint8_t *p)
{
    return static_cast<uint16_t>(p[0] | (p[1] << 8));
}

// The largest image of the icon this executable carries (its resource 1, from vibin.rc.in): its data, or empty
std::vector<uint8_t> embeddedIcon()
{
    const HRSRC group = FindResourceW(nullptr, MAKEINTRESOURCEW(1), MAKEINTRESOURCEW(14));
    if (!group)
        return {};
    const HGLOBAL loaded = LoadResource(nullptr, group);
    const auto *directory = loaded ? static_cast<const uint8_t *>(LockResource(loaded)) : nullptr;
    const DWORD size = SizeofResource(nullptr, group);
    if (!directory || size < 6)
        return {};
    const size_t count = le16(directory + 4);
    int widest = -1;
    int id = 0;
    for (size_t i = 0; i < count && 6 + 14 * (i + 1) <= size; ++i) {
        const uint8_t *entry = directory + 6 + 14 * i;
        const int width = entry[0] == 0 ? 256 : entry[0]; // 0 stands for 256
        if (width > widest) {
            widest = width;
            id = le16(entry + 12);
        }
    }
    const HRSRC image = id ? FindResourceW(nullptr, MAKEINTRESOURCEW(id), MAKEINTRESOURCEW(3)) : nullptr;
    const HGLOBAL data = image ? LoadResource(nullptr, image) : nullptr;
    const auto *bytes = data ? static_cast<const uint8_t *>(LockResource(data)) : nullptr;
    if (!bytes)
        return {};
    return std::vector<uint8_t>(bytes, bytes + SizeofResource(nullptr, image));
}

std::string narrow(const std::wstring &text)
{
    const int n = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>((std::max)(0, n - 1)), '\0');
    if (n > 1)
        WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1, out.data(), n, nullptr, nullptr);
    return out;
}

void put16(std::vector<uint8_t> &out, uint32_t v)
{
    out.push_back(static_cast<uint8_t>(v & 0xFF));
    out.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
}

void put32(std::vector<uint8_t> &out, uint32_t v)
{
    put16(out, v & 0xFFFF);
    put16(out, v >> 16);
}

// The bitmap as the data of an icon image: a DIB (its header, the 32-bit pixels bottom up with their alpha,
// and a mask of zeros, the alpha being what counts). Its size depends only on `size`, so another icon's
// image fits exactly where this one is, which is how the executable's icon is replaced (IconPatch.cpp).
bool encodeDib(Gdiplus::Bitmap &bitmap, int size, std::vector<uint8_t> &out)
{
    Gdiplus::BitmapData data{};
    const Gdiplus::Rect all(0, 0, size, size);
    if (bitmap.LockBits(&all, Gdiplus::ImageLockModeRead, PixelFormat32bppARGB, &data) != Gdiplus::Ok)
        return false;
    const uint32_t pixels = static_cast<uint32_t>(size) * static_cast<uint32_t>(size) * 4;
    const uint32_t maskRow = ((static_cast<uint32_t>(size) + 31) / 32) * 4;
    out.clear();
    put32(out, 40);                                 // the header's size
    put32(out, static_cast<uint32_t>(size));
    put32(out, static_cast<uint32_t>(size) * 2);    // the pixels and the mask
    put16(out, 1);
    put16(out, 32);
    put32(out, 0);                                  // BI_RGB
    put32(out, pixels + maskRow * static_cast<uint32_t>(size));
    for (int i = 0; i < 4; ++i)
        put32(out, 0);
    for (int y = size - 1; y >= 0; --y) {
        const auto *row = static_cast<const uint8_t *>(data.Scan0) + static_cast<ptrdiff_t>(y) * data.Stride;
        out.insert(out.end(), row, row + size * 4);
    }
    out.insert(out.end(), static_cast<size_t>(maskRow) * static_cast<size_t>(size), 0);
    bitmap.UnlockBits(&data);
    return true;
}

}

void drawLogoMark(Gdiplus::Graphics &g, float cx, float cy, float r, ui::Color base, ui::Color accent, bool small)
{
    const float k = r / 42.0f; // the loading screen's disc has radius 42
    const ui::Color black{0.0f, 0.0f, 0.0f, 1.0f}, white{1.0f, 1.0f, 1.0f, 1.0f};
    const ui::Color shadow = mix(base, black, 0.55f);
    const ui::Color light = mix(base, white, 0.10f);

    // Raised: a shadow below and to the right, a highlight above and to the left, and the dome
    circleShadow(g, shadow, cx + 4.0f * k, cy + 5.0f * k, r, 12.0f * k, 0.9f);
    circleShadow(g, light, cx - 4.0f * k, cy - 4.0f * k, r, 8.0f * k, 0.5f);
    Gdiplus::LinearGradientBrush dome(Gdiplus::PointF(cx - r, cy - r), Gdiplus::PointF(cx + r, cy + r),
                                      gdi(mix(base, light, 0.9f)), gdi(mix(base, shadow, 0.12f)));
    fillCircle(g, dome, cx, cy, r);

    // The waveform
    Gdiplus::SolidBrush bars(gdi(accent));
    const float five[] = {0.32f, 0.62f, 1.0f, 0.7f, 0.4f};
    const float three[] = {0.62f, 1.0f, 0.7f};
    const float *heights = small ? three : five;
    const int count = small ? 3 : 5;
    const float width = (small ? 11.0f : 4.6f) * k, height = (small ? 36.0f : 30.0f) * k, step = (small ? 18.0f : 8.0f) * k;
    for (int i = 0; i < count; ++i) {
        const float bh = height * heights[i];
        const float bx = cx + (static_cast<float>(i) - static_cast<float>(count - 1) * 0.5f) * step - width * 0.5f;
        Gdiplus::GraphicsPath bar;
        roundedRect(bar, bx, cy - bh * 0.5f, width, bh, width * 0.5f - 0.01f);
        g.FillPath(&bars, &bar);
    }
}

bool setWindowIcon(void *window, ui::Color base, ui::Color accent)
{
    HWND hwnd = static_cast<HWND>(window);
    if (!hwnd)
        return false;
    // GDI+ stays started for the life of the process (never shut down: the process's end does it), so
    // making the icon again as a colour is dragged costs the drawing and not the start-up
    static const GdiplusSession *gdiplus = new GdiplusSession;
    if (!gdiplus->ok)
        return false;
    auto make = [&](int pixels) -> HICON {
        HICON icon = nullptr;
        auto bitmap = renderIcon(pixels, base, accent);
        if (bitmap->GetHICON(&icon) != Gdiplus::Ok)
            return nullptr;
        return icon;
    };
    const HICON big = make((std::max)(32, GetSystemMetrics(SM_CXICON)));
    const HICON small = make((std::max)(16, GetSystemMetrics(SM_CXSMICON)));
    if (!big || !small) {
        if (big)
            DestroyIcon(big);
        if (small)
            DestroyIcon(small);
        return false;
    }
    SendMessageW(hwnd, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(big));
    SendMessageW(hwnd, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(small));
    // The icons it replaced are ours to free (one window gets them)
    static HICON currentBig = nullptr, currentSmall = nullptr;
    if (currentBig)
        DestroyIcon(currentBig);
    if (currentSmall)
        DestroyIcon(currentSmall);
    currentBig = big;
    currentSmall = small;
    return true;
}

bool writeIconFile(const std::string &path, ui::Color base, ui::Color accent, std::string &error)
{
    GdiplusSession gdiplus;
    if (!gdiplus.ok) {
        error = "GDI+ isn't available";
        return false;
    }
    const int sizes[] = {16, 24, 32, 48, 64, 128, 256};
    std::vector<std::vector<uint8_t>> images;
    for (int size : sizes) {
        auto bitmap = renderIcon(size, base, accent);
        std::vector<uint8_t> bytes;
        if (!encodeDib(*bitmap, size, bytes)) {
            error = "Couldn't encode the " + std::to_string(size) + " pixel image";
            return false;
        }
        images.push_back(std::move(bytes));
    }

    // The file: the directory (type 1, a count), an entry for each image, then the images (DIBs, not PNG: an
    // image's size then depends on its size in pixels only, so the executable's icon can be replaced byte
    // for byte, see IconPatch.cpp; 0 stands for 256 in an entry's width and height)
    std::vector<uint8_t> file;
    put16(file, 0);
    put16(file, 1);
    put16(file, static_cast<uint32_t>(images.size()));
    uint32_t offset = 6 + 16 * static_cast<uint32_t>(images.size());
    for (size_t i = 0; i < images.size(); ++i) {
        const int size = sizes[i];
        file.push_back(static_cast<uint8_t>(size >= 256 ? 0 : size));
        file.push_back(static_cast<uint8_t>(size >= 256 ? 0 : size));
        file.push_back(0); // no palette
        file.push_back(0);
        put16(file, 1);  // planes
        put16(file, 32); // bits a pixel
        put32(file, static_cast<uint32_t>(images[i].size()));
        put32(file, offset);
        offset += static_cast<uint32_t>(images[i].size());
    }
    for (const auto &image : images)
        file.insert(file.end(), image.begin(), image.end());

    std::ofstream out(std::filesystem::u8path(path), std::ios::binary);
    out.write(reinterpret_cast<const char *>(file.data()), static_cast<std::streamsize>(file.size()));
    if (!out) {
        error = "Couldn't write " + path;
        return false;
    }
    return true;
}

bool scheduleExecutableIcon(ui::Color base, ui::Color accent)
{
    wchar_t buffer[MAX_PATH * 4] = {};
    const DWORD length = GetModuleFileNameW(nullptr, buffer, static_cast<DWORD>(sizeof(buffer) / sizeof(buffer[0])));
    if (length == 0 || length >= sizeof(buffer) / sizeof(buffer[0]))
        return false;
    const std::wstring exe(buffer, length);
    const size_t slash = exe.find_last_of(L"\\/");
    const std::wstring patcher = (slash == std::wstring::npos ? std::wstring() : exe.substr(0, slash + 1)) + L"vibin-icon-patch.exe";
    if (GetFileAttributesW(patcher.c_str()) == INVALID_FILE_ATTRIBUTES)
        return false;

    // Is the icon in the executable already this one? Its largest image against the one drawn now (the
    // same drawing in the same colours gives the same pixels, so equal means nothing to do)
    {
        GdiplusSession gdiplus;
        std::vector<uint8_t> wanted;
        if (!gdiplus.ok || !encodeDib(*renderIcon(256, base, accent), 256, wanted))
            return false;
        if (wanted == embeddedIcon())
            return false;
    }

    // The icon as a file for the patcher (it deletes it), then the patcher, free of this process: it
    // waits for this one to end
    wchar_t temp[MAX_PATH + 1] = {};
    if (GetTempPathW(MAX_PATH, temp) == 0)
        return false;
    const std::wstring ico = std::wstring(temp) + L"vibin-icon-" + std::to_wstring(GetCurrentProcessId()) + L".ico";
    std::string error;
    if (!writeIconFile(narrow(ico), base, accent, error))
        return false;
    std::wstring command = L"\"" + patcher + L"\" " + std::to_wstring(GetCurrentProcessId()) + L" \"" + exe + L"\" \"" + ico + L"\"";
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    // Out of this process's job if it has one that would end the patcher with it (a terminal's, say)
    bool started = CreateProcessW(patcher.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_BREAKAWAY_FROM_JOB | DETACHED_PROCESS,
                                  nullptr, nullptr, &startup, &process) != 0;
    if (!started)
        started = CreateProcessW(patcher.c_str(), command.data(), nullptr, nullptr, FALSE, DETACHED_PROCESS, nullptr, nullptr, &startup,
                                 &process) != 0;
    if (!started) {
        DeleteFileW(ico.c_str());
        return false;
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return true;
}

void keepConsoleOffTaskbar()
{
    HWND console = GetConsoleWindow();
    if (!console)
        return;
    // Only when this program is the console's only user: with a shell or a terminal's it is theirs
    DWORD users[2] = {};
    if (GetConsoleProcessList(users, 2) != 1)
        return;
    // A tool window has no taskbar button; the style takes hold when the window is hidden and shown again
    const LONG_PTR style = GetWindowLongPtrW(console, GWL_EXSTYLE);
    ShowWindow(console, SW_HIDE);
    SetWindowLongPtrW(console, GWL_EXSTYLE, (style | WS_EX_TOOLWINDOW) & ~static_cast<LONG_PTR>(WS_EX_APPWINDOW));
    ShowWindow(console, SW_SHOWNA);
}

#else

bool setWindowIcon(void *, ui::Color, ui::Color)
{
    return false;
}

bool scheduleExecutableIcon(ui::Color, ui::Color)
{
    return false;
}

void keepConsoleOffTaskbar() {}

bool writeIconFile(const std::string &, ui::Color, ui::Color, std::string &error)
{
    error = "Icons are made on Windows only";
    return false;
}

#endif

}
