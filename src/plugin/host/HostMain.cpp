// vybn-plugin-host.exe: runs one plugin for the app, sandboxed, CLAP or VST3 by the file it is
// given (host/ClapFormat.cpp, host/Vst3Format.cpp; everything here is either's). The app starts it
// (in a job object and, where Windows allows, an AppContainer with no capabilities) and hands it,
// as inherited handles on the command line:
//
//   vybn-plugin-host <control-in> <control-out> <shared-memory> <process-event> <done-event>
//
// Control messages (plugin/Protocol.h) arrive on control-in and are answered on control-out, on
// this program's main thread, which is the plugin's main thread. The plugin's audio thread waits
// on the process event, runs one block from the shared memory and signals the done event. A
// plugin's interface either opens as its own floating window, or (embedded) is parented to a frame
// window of this process that sits exactly over where the app draws it, invisible (layered, 1/255
// opaque), just above the app's window in the stacking order (never topmost, so other programs'
// windows stay above it); the app moves it there itself, since from the sandbox the app's windows
// can't be seen. The real mouse and keyboard reach the plugin there directly, so it behaves as it
// would anywhere (its menus, lists and dialogs, top-level windows of its own, open at the right
// place at once). It is captured (60 times a second while it is used, 25 otherwise) into shared
// memory for the app to draw beneath it. The app's mouse and keys can be replayed onto it too
// (while the frame is stood aside). Nothing of the plugin's is ever parented into the app's window
// (which would tie the app's input to a sandboxed, possibly hung, thread).
//
// The host exits when the app says Quit or its control pipe breaks (the app went away); the job
// object kills it anyway when the app closes it.

#include "plugin/host/Host.h"

#include <xmmintrin.h>

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>

namespace host {

using namespace plugin;

// --- The process's handles and state

Shared *shared = nullptr;

std::atomic<bool> restartRequested{false};
std::atomic<bool> latencyChanged{false};
std::atomic<bool> stateDirty{false};
std::atomic<uint32_t> paramsRescan{0};
std::atomic<bool> guiClosed{false};

namespace {

HANDLE controlIn = nullptr;
HANDLE controlOut = nullptr;
HANDLE processEvent = nullptr;
HANDLE doneEvent = nullptr;

HANDLE messageEvent = nullptr;  // the reader thread has a message
HANDLE callbackEvent = nullptr; // the plugin's threads left news
HANDLE wakeAudio = nullptr;     // the main thread wants the audio thread
HANDLE audioAck = nullptr;      // the audio thread did what it was asked

DWORD mainThreadId = 0;
std::atomic<DWORD> audioThreadId{0};

struct Incoming {
    MessageHeader header{};
    std::string payload;
    bool broken = false; // the pipe closed
};
std::mutex incomingMutex;
std::deque<Incoming> incoming;

std::mutex newsMutex;
std::vector<std::pair<int, std::string>> logs;

// The plugin, once a request has named its file
std::unique_ptr<Format> format;
bool pluginLoaded = false;
double sampleRate = 48000.0;
std::atomic<bool> active{false};
std::atomic<bool> paused{false}; // the audio thread answers with silence (while the main thread works)
std::atomic<bool> stopRequest{false};

// The embedded interface: the window it is parented to, the capture and where it goes
struct Embedded {
    HWND frameWindow = nullptr;
    GuiFrame *frame = nullptr;
    HDC captureDC = nullptr;
    HBITMAP captureBitmap = nullptr;
    void *captureBits = nullptr;
    uint32_t captureWidth = 0, captureHeight = 0;
    ULONGLONG lastCapture = 0;
    ULONGLONG busyUntil = 0;   // used recently: captured more often
    uint32_t mouseButtons = 0; // MK_ flags held
    HWND lastTarget = nullptr;
};
Embedded embedded;

}

void wake() { SetEvent(callbackEvent); }

void log(int severity, const std::string &text)
{
    std::lock_guard<std::mutex> lock(newsMutex);
    if (logs.size() < 200)
        logs.emplace_back(severity, text.substr(0, 2000));
}

bool isMainThread() { return GetCurrentThreadId() == mainThreadId; }
bool isAudioThread() { return GetCurrentThreadId() == audioThreadId.load(); }
bool pluginActive() { return active; }

std::wstring widen(const std::string &s)
{
    if (s.empty())
        return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), &w[0], n);
    return w;
}

std::string narrow(const std::wstring &s)
{
    if (s.empty())
        return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), &out[0], n, nullptr, nullptr);
    return out;
}

void silence(uint32_t frames)
{
    for (uint32_t c = 0; c < kMaxChannels; ++c)
        std::memset(shared->output[c], 0, frames * sizeof(float));
    shared->outputChannels = kMaxChannels;
}

bool resizeFrame(uint32_t width, uint32_t height)
{
    if (!embedded.frameWindow)
        return false;
    width = width < 16 ? 16 : (width > kGuiMaxWidth ? kGuiMaxWidth : width);
    height = height < 16 ? 16 : (height > kGuiMaxHeight ? kGuiMaxHeight : height);
    SetWindowPos(embedded.frameWindow, nullptr, 0, 0, static_cast<int>(width), static_cast<int>(height), SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    return true;
}

namespace {

// --- Pipes

bool readAll(HANDLE h, void *data, DWORD size)
{
    char *p = static_cast<char *>(data);
    while (size > 0) {
        DWORD got = 0;
        if (!ReadFile(h, p, size, &got, nullptr) || got == 0)
            return false;
        p += got;
        size -= got;
    }
    return true;
}

bool writeAll(HANDLE h, const void *data, DWORD size)
{
    const char *p = static_cast<const char *>(data);
    while (size > 0) {
        DWORD put = 0;
        if (!WriteFile(h, p, size, &put, nullptr) || put == 0)
            return false;
        p += put;
        size -= put;
    }
    return true;
}

// Main thread only
void send(Message type, uint32_t sequence, const std::string &payload = {})
{
    MessageHeader header{static_cast<uint32_t>(type), sequence, static_cast<uint32_t>(payload.size())};
    if (!writeAll(controlOut, &header, sizeof header) || !writeAll(controlOut, payload.data(), static_cast<DWORD>(payload.size())))
        ExitProcess(3); // the app has gone
}

void fail(uint32_t sequence, const std::string &why)
{
    Writer w;
    w.str(why);
    send(Message::Fail, sequence, w.data);
}

void readerThread()
{
    for (;;) {
        Incoming in;
        if (!readAll(controlIn, &in.header, sizeof in.header) || in.header.size > kMaxMessageBytes) {
            in.broken = true;
        } else {
            in.payload.resize(in.header.size);
            if (in.header.size > 0 && !readAll(controlIn, &in.payload[0], in.header.size))
                in.broken = true;
        }
        const bool broken = in.broken;
        {
            std::lock_guard<std::mutex> lock(incomingMutex);
            incoming.push_back(std::move(in));
        }
        SetEvent(messageEvent);
        if (broken)
            return;
    }
}

// --- Audio thread

void runBlock()
{
    const uint32_t frames = shared->frames <= kMaxFrames ? shared->frames : kMaxFrames;
    if (!active || paused) {
        silence(frames);
        shared->status = kProcessSleep;
        return;
    }
    format->process(frames);
}

void audioThread()
{
    audioThreadId = GetCurrentThreadId();
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    _mm_setcsr(_mm_getcsr() | 0x8040); // flush denormals to zero
    uint32_t last = shared->done.load(std::memory_order_relaxed);
    HANDLE waits[2] = {processEvent, wakeAudio};
    for (;;) {
        WaitForMultipleObjects(2, waits, FALSE, INFINITE);
        if (stopRequest) {
            if (format)
                format->stopProcessing();
            stopRequest = false;
            SetEvent(audioAck);
        }
        const uint32_t request = shared->request.load(std::memory_order_acquire);
        if (request == last)
            continue;
        runBlock();
        last = request;
        shared->done.store(request, std::memory_order_release);
        SetEvent(doneEvent);
    }
}

// Asks the audio thread to stop processing and waits until it has (main thread)
void stopAudio()
{
    paused = true;
    stopRequest = true;
    SetEvent(wakeAudio);
    WaitForSingleObject(audioAck, 5000);
}

// --- Main thread: the requests

bool endsWith(const std::string &s, const std::string &tail)
{
    if (s.size() < tail.size())
        return false;
    for (size_t i = 0; i < tail.size(); ++i)
        if (std::tolower(static_cast<unsigned char>(s[s.size() - tail.size() + i])) != tail[i])
            return false;
    return true;
}

// The format for a plugin's file, made and the file opened on the first request
bool openPlugin(const std::string &path, std::string &error)
{
    if (!format) {
        std::string trimmed = path;
        while (!trimmed.empty() && (trimmed.back() == '\\' || trimmed.back() == '/'))
            trimmed.pop_back();
        format = endsWith(trimmed, ".vst3") ? makeVst3Format() : makeClapFormat();
    }
    return format->open(path, error);
}

void handleScan(uint32_t sequence, Reader &r)
{
    const std::string path = r.str();
    std::string error;
    std::vector<Descriptor> found;
    if (!openPlugin(path, error) || !format->scan(found, error))
        return fail(sequence, error);
    Writer w;
    w.u32(static_cast<uint32_t>(found.size()));
    for (const Descriptor &d : found)
        writeDescriptor(w, d);
    send(Message::Reply, sequence, w.data);
}

// The sidechain input of one plugin, in a host of its own (the scan's descriptors are already safe)
void handleProbe(uint32_t sequence, Reader &r)
{
    const std::string path = r.str();
    const std::string id = r.str();
    std::string error;
    uint32_t channels = 0;
    if (!openPlugin(path, error) || !format->probe(id, channels, error))
        return fail(sequence, error);
    Writer w;
    w.u32(channels);
    send(Message::Reply, sequence, w.data);
}

void handleLoad(uint32_t sequence, Reader &r)
{
    const std::string path = r.str();
    const std::string id = r.str();
    if (pluginLoaded)
        return fail(sequence, "A plugin is already loaded");
    std::string error;
    LoadedInfo info;
    if (!openPlugin(path, error) || !format->load(id, info, error))
        return fail(sequence, error);
    pluginLoaded = true;
    Writer w;
    w.str(info.name);
    w.u32(info.inputChannels);
    w.u32(info.outputChannels);
    w.u32(info.notePorts);
    w.u32(info.params);
    w.u32(info.state);
    w.u32((info.gui ? 1u : 0u) | (info.embedded ? 2u : 0u));
    send(Message::Reply, sequence, w.data);
}

void releaseCapture()
{
    if (embedded.captureBitmap)
        DeleteObject(embedded.captureBitmap);
    if (embedded.captureDC)
        DeleteDC(embedded.captureDC);
    embedded.captureBitmap = nullptr;
    embedded.captureDC = nullptr;
    embedded.captureBits = nullptr;
    embedded.captureWidth = embedded.captureHeight = 0;
}

void closeGui()
{
    if (pluginLoaded)
        format->guiClose();
    if (embedded.frameWindow) {
        DestroyWindow(embedded.frameWindow);
        embedded.frameWindow = nullptr;
    }
    releaseCapture();
    if (embedded.frame) {
        UnmapViewOfFile(embedded.frame);
        embedded.frame = nullptr;
    }
    embedded.mouseButtons = 0;
    embedded.lastTarget = nullptr;
}

// Where the frame waits until the app says where it is shown (and while it isn't): off every
// screen, still shown so the system keeps drawing it
const int kAwayX = -32000 + 8000;
const int kAwayY = -32000 + 8000;

// Where the frame's top-left corner is on the desktop now
POINT frameOrigin()
{
    POINT p{0, 0};
    if (embedded.frameWindow)
        ClientToScreen(embedded.frameWindow, &p);
    return p;
}

// The frame's window procedure: a press anywhere in the interface brings it to the front (the
// app allows this while the mouse is over the interface), so the keyboard reaches the plugin as it
// would in any host, and the interface keeps the press where the plugin put its focus
LRESULT CALLBACK frameProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    if (message == WM_PARENTNOTIFY) {
        const UINT what = LOWORD(wParam);
        if ((what == WM_LBUTTONDOWN || what == WM_RBUTTONDOWN || what == WM_MBUTTONDOWN) && GetForegroundWindow() != hwnd)
            SetForegroundWindow(hwnd);
    } else if (message == WM_MOUSEACTIVATE) {
        return MA_ACTIVATE;
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

void handleGuiEmbed(uint32_t sequence, Reader &r)
{
    HANDLE mapping = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(r.u32()));
    if (!pluginLoaded) {
        CloseHandle(mapping);
        return fail(sequence, "This plugin has no interface");
    }
    closeGui();
    embedded.frame = static_cast<GuiFrame *>(MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(GuiFrame)));
    CloseHandle(mapping);
    if (!embedded.frame)
        return fail(sequence, "Couldn't reach the app's picture of the interface");
    static bool registered = false;
    if (!registered) {
        WNDCLASSW wc{};
        wc.lpfnWndProc = frameProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hbrBackground = reinterpret_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
        wc.lpszClassName = L"VybnPluginFrame";
        registered = RegisterClassW(&wc) != 0;
    }
    // Invisible but taking the mouse: the app shows the picture beneath it
    embedded.frameWindow = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_LAYERED, L"VybnPluginFrame", L"",
                                           WS_POPUP | WS_CLIPCHILDREN, kAwayX, kAwayY, 640, 400, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!embedded.frameWindow) {
        closeGui();
        return fail(sequence, "Couldn't make a window for the interface");
    }
    SetLayeredWindowAttributes(embedded.frameWindow, 0, 1, LWA_ALPHA);
    ShowWindow(embedded.frameWindow, SW_SHOWNOACTIVATE);
    std::string error;
    if (!format->guiEmbed(embedded.frameWindow, error)) {
        closeGui();
        return fail(sequence, error);
    }
    embedded.lastCapture = 0;
    RECT rc;
    GetClientRect(embedded.frameWindow, &rc);
    Writer w;
    w.u32(static_cast<uint32_t>(rc.right - rc.left));
    w.u32(static_cast<uint32_t>(rc.bottom - rc.top));
    // The app places and stacks the frame itself: from the sandbox the app's windows can't even
    // be seen, let alone stacked against, while the app may move this process's windows
    w.u32(static_cast<uint32_t>(reinterpret_cast<uintptr_t>(embedded.frameWindow)));
    send(Message::Reply, sequence, w.data);
}

// Copies the interface into the shared frame: 60 times a second while it is used, 25 otherwise
void captureGui()
{
    if (!embedded.frameWindow || !embedded.frame)
        return;
    const ULONGLONG now = GetTickCount64();
    if (now - embedded.lastCapture < (now < embedded.busyUntil ? 15u : 40u))
        return;
    embedded.lastCapture = now;
    RECT rc;
    GetClientRect(embedded.frameWindow, &rc);
    const uint32_t width = static_cast<uint32_t>(rc.right - rc.left) < kGuiMaxWidth ? static_cast<uint32_t>(rc.right - rc.left) : kGuiMaxWidth;
    const uint32_t height = static_cast<uint32_t>(rc.bottom - rc.top) < kGuiMaxHeight ? static_cast<uint32_t>(rc.bottom - rc.top) : kGuiMaxHeight;
    if (width == 0 || height == 0)
        return;
    if (width != embedded.captureWidth || height != embedded.captureHeight) {
        releaseCapture();
        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof info.bmiHeader;
        info.bmiHeader.biWidth = static_cast<LONG>(width);
        info.bmiHeader.biHeight = -static_cast<LONG>(height); // top-down
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        HDC screen = GetDC(nullptr);
        embedded.captureDC = CreateCompatibleDC(screen);
        embedded.captureBitmap = CreateDIBSection(screen, &info, DIB_RGB_COLORS, &embedded.captureBits, nullptr, 0);
        ReleaseDC(nullptr, screen);
        if (!embedded.captureBitmap) {
            releaseCapture();
            return;
        }
        SelectObject(embedded.captureDC, embedded.captureBitmap);
        embedded.captureWidth = width;
        embedded.captureHeight = height;
    }
    // The system's copy of the window, children included, read without making the plugin draw
    // again. If that comes out one flat colour (the frame not drawn yet, or off the screen), the
    // window is asked to paint into the capture instead, which costs a full redraw.
    HDC source = GetDCEx(embedded.frameWindow, nullptr, DCX_WINDOW | DCX_CACHE);
    BitBlt(embedded.captureDC, 0, 0, static_cast<int>(width), static_cast<int>(height), source, 0, 0, SRCCOPY);
    ReleaseDC(embedded.frameWindow, source);
    GdiFlush();
    auto isFlat = [&] {
        const uint32_t *px = static_cast<const uint32_t *>(embedded.captureBits);
        for (size_t i = 1, n = static_cast<size_t>(width) * height; i < n; i += 7)
            if ((px[i] & 0xFFFFFF) != (px[0] & 0xFFFFFF))
                return false;
        return true;
    };
    if (isFlat()) {
        PrintWindow(embedded.frameWindow, embedded.captureDC, 0);
        GdiFlush();
    }
    GuiFrame &f = *embedded.frame;
    const uint32_t seq = f.sequence.load(std::memory_order_relaxed);
    f.sequence.store(seq | 1, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
    f.width = width;
    f.height = height;
    std::memcpy(f.pixels, embedded.captureBits, static_cast<size_t>(width) * height * 4);
    std::atomic_thread_fence(std::memory_order_release);
    f.sequence.store((seq | 1) + 1, std::memory_order_release);
}

// The deepest visible child of the frame under a point (frame pixels), or the window holding the
// mouse while a drag is under way
HWND inputTarget(POINT at)
{
    if (HWND captured = GetCapture())
        return captured;
    HWND w = embedded.frameWindow;
    for (int depth = 0; depth < 32; ++depth) {
        POINT local = at;
        MapWindowPoints(embedded.frameWindow, w, &local, 1);
        HWND child = ChildWindowFromPointEx(w, local, CWP_SKIPINVISIBLE | CWP_SKIPDISABLED | CWP_SKIPTRANSPARENT);
        if (!child || child == w)
            break;
        w = child;
    }
    return w;
}

void handleGuiInput(Reader &r)
{
    GuiInput in;
    in.kind = static_cast<GuiInputKind>(r.u32());
    in.x = static_cast<int32_t>(r.u32());
    in.y = static_cast<int32_t>(r.u32());
    in.button = r.u32();
    in.mods = r.u32();
    in.wheel = static_cast<int32_t>(r.u32());
    in.key = r.u32();
    if (!r.ok() || !embedded.frameWindow)
        return;
    embedded.busyUntil = GetTickCount64() + 1000;
    const POINT at{in.x, in.y};
    const POINT origin = frameOrigin();
    WPARAM held = embedded.mouseButtons;
    if (in.mods & 1)
        held |= MK_SHIFT;
    if (in.mods & 2)
        held |= MK_CONTROL;
    auto toTarget = [&](HWND target) {
        POINT local = at;
        MapWindowPoints(embedded.frameWindow, target, &local, 1);
        return MAKELPARAM(static_cast<short>(local.x), static_cast<short>(local.y));
    };
    static const UINT downs[3] = {WM_LBUTTONDOWN, WM_RBUTTONDOWN, WM_MBUTTONDOWN};
    static const UINT ups[3] = {WM_LBUTTONUP, WM_RBUTTONUP, WM_MBUTTONUP};
    static const UINT doubles[3] = {WM_LBUTTONDBLCLK, WM_RBUTTONDBLCLK, WM_MBUTTONDBLCLK};
    static const WPARAM flags[3] = {MK_LBUTTON, MK_RBUTTON, MK_MBUTTON};
    const uint32_t b = in.button < 3 ? in.button : 0;
    switch (in.kind) {
    case GuiInputKind::Move: {
        HWND target = inputTarget(at);
        if (embedded.lastTarget && embedded.lastTarget != target && IsWindow(embedded.lastTarget))
            PostMessageW(embedded.lastTarget, WM_MOUSELEAVE, 0, 0);
        embedded.lastTarget = target;
        PostMessageW(target, WM_MOUSEMOVE, held, toTarget(target));
        break;
    }
    case GuiInputKind::Down:
    case GuiInputKind::DoubleClick: {
        HWND target = inputTarget(at);
        embedded.mouseButtons |= static_cast<uint32_t>(flags[b]);
        const bool dbl = in.kind == GuiInputKind::DoubleClick && (GetClassLongW(target, GCL_STYLE) & CS_DBLCLKS);
        PostMessageW(target, dbl ? doubles[b] : downs[b], held | flags[b], toTarget(target));
        break;
    }
    case GuiInputKind::Up: {
        HWND target = inputTarget(at);
        embedded.mouseButtons &= ~static_cast<uint32_t>(flags[b]);
        PostMessageW(target, ups[b], held & ~flags[b], toTarget(target));
        if (b == 1)
            PostMessageW(target, WM_CONTEXTMENU, reinterpret_cast<WPARAM>(target), MAKELPARAM(origin.x + at.x, origin.y + at.y));
        break;
    }
    case GuiInputKind::Wheel: {
        HWND target = GetFocus() ? GetFocus() : inputTarget(at);
        // The wheel's point is on the screen: where the frame sits
        PostMessageW(target, WM_MOUSEWHEEL, MAKEWPARAM(held, static_cast<short>(in.wheel)), MAKELPARAM(origin.x + at.x, origin.y + at.y));
        break;
    }
    case GuiInputKind::Leave:
        if (embedded.lastTarget && IsWindow(embedded.lastTarget))
            PostMessageW(embedded.lastTarget, WM_MOUSELEAVE, 0, 0);
        embedded.lastTarget = nullptr;
        break;
    case GuiInputKind::KeyDown:
    case GuiInputKind::KeyUp:
    case GuiInputKind::Char: {
        HWND target = GetFocus();
        if (!target || (target != embedded.frameWindow && !IsChild(embedded.frameWindow, target)))
            target = embedded.lastTarget && IsWindow(embedded.lastTarget) ? embedded.lastTarget : inputTarget(at);
        const UINT scan = MapVirtualKeyW(in.key, MAPVK_VK_TO_VSC);
        if (in.kind == GuiInputKind::Char)
            PostMessageW(target, WM_CHAR, in.key, 1);
        else if (in.kind == GuiInputKind::KeyDown)
            PostMessageW(target, WM_KEYDOWN, in.key, 1 | (scan << 16));
        else
            PostMessageW(target, WM_KEYUP, in.key, 1 | (scan << 16) | (1u << 30) | (1u << 31));
        break;
    }
    }
}

// Only the part of the interface the app shows takes the mouse: the rest of the frame is cut away
void handleGuiClip(Reader &r)
{
    const int32_t x = static_cast<int32_t>(r.u32()), y = static_cast<int32_t>(r.u32());
    const int32_t w = static_cast<int32_t>(r.u32()), h = static_cast<int32_t>(r.u32());
    if (!r.ok() || !embedded.frameWindow)
        return;
    if (w <= 0 || h <= 0)
        SetWindowRgn(embedded.frameWindow, nullptr, TRUE);
    else
        SetWindowRgn(embedded.frameWindow, CreateRectRgn(x, y, x + w, y + h), TRUE); // the system keeps the region
}

void handleGuiResize(Reader &r)
{
    const uint32_t width = r.u32(), height = r.u32();
    if (!r.ok() || !embedded.frameWindow || !pluginLoaded)
        return;
    format->guiResize(width, height);
}

void handleGuiOpen(uint32_t sequence)
{
    if (!pluginLoaded)
        return fail(sequence, "This plugin has no window of its own");
    std::string error;
    if (!format->guiOpen(error))
        return fail(sequence, error);
    send(Message::Reply, sequence);
}

bool activate(double rate, std::string &error)
{
    sampleRate = rate;
    if (!format->activate(rate, error))
        return false;
    paused = false;
    active = true;
    return true;
}

void deactivate()
{
    if (!active)
        return;
    stopAudio();
    active = false;
    format->deactivate();
    paused = false;
}

void handleActivate(uint32_t sequence, Reader &r)
{
    const double rate = r.f64();
    if (!pluginLoaded)
        return fail(sequence, "No plugin is loaded");
    deactivate();
    std::string error;
    if (!activate(rate > 1000.0 && rate < 1e6 ? rate : 48000.0, error))
        return fail(sequence, error);
    Writer w;
    w.u32(shared->latency.load());
    send(Message::Reply, sequence, w.data);
}

void handleParams(uint32_t sequence)
{
    const std::vector<ParamInfo> list = pluginLoaded ? format->params() : std::vector<ParamInfo>();
    Writer w;
    w.u32(static_cast<uint32_t>(list.size()));
    for (const ParamInfo &p : list)
        writeParam(w, p);
    send(Message::Reply, sequence, w.data);
}

void handleParamText(uint32_t sequence, Reader &r)
{
    const uint32_t id = r.u32();
    const double value = r.f64();
    std::string text;
    if (!pluginLoaded || !format->paramText(id, value, text))
        return fail(sequence, "No text");
    Writer w;
    w.str(text);
    send(Message::Reply, sequence, w.data);
}

void handleSaveState(uint32_t sequence)
{
    if (!pluginLoaded || !format->hasState())
        return send(Message::Reply, sequence, std::string());
    std::string data, error;
    if (!format->saveState(data, error))
        return fail(sequence, error);
    send(Message::Reply, sequence, data);
}

void handleLoadState(uint32_t sequence, const std::string &payload)
{
    if (!pluginLoaded || !format->hasState())
        return fail(sequence, "The plugin has no state to load");
    std::string error;
    if (!format->loadState(payload, error))
        return fail(sequence, error);
    send(Message::Reply, sequence);
}

void quit()
{
    closeGui();
    if (pluginLoaded)
        deactivate();
    if (format)
        format->unload();
    ExitProcess(0);
}

void handle(const Incoming &in)
{
    if (in.broken)
        quit();
    Reader r(in.payload);
    const uint32_t sequence = in.header.sequence;
    switch (static_cast<Message>(in.header.type)) {
    case Message::Scan:
        return handleScan(sequence, r);
    case Message::Probe:
        return handleProbe(sequence, r);
    case Message::Load:
        return handleLoad(sequence, r);
    case Message::Activate:
        return handleActivate(sequence, r);
    case Message::Deactivate:
        if (pluginLoaded)
            deactivate();
        return send(Message::Reply, sequence);
    case Message::Params:
        return handleParams(sequence);
    case Message::ParamText:
        return handleParamText(sequence, r);
    case Message::SaveState:
        return handleSaveState(sequence);
    case Message::LoadState:
        return handleLoadState(sequence, in.payload);
    case Message::Ping:
        return send(Message::Reply, sequence);
    case Message::GuiOpen:
        return handleGuiOpen(sequence);
    case Message::GuiClose:
        closeGui();
        return send(Message::Reply, sequence);
    case Message::GuiEmbed:
        return handleGuiEmbed(sequence, r);
    case Message::GuiInput:
        return handleGuiInput(r); // no reply: it is sent as the mouse moves
    case Message::GuiResize:
        return handleGuiResize(r);
    case Message::GuiClip:
        return handleGuiClip(r);
    case Message::Quit:
        quit();
        return;
    default:
        return fail(sequence, "Unknown request");
    }
}

// Passes on what the plugin's threads asked for
void housekeeping()
{
    if (!pluginLoaded)
        return;
    format->housekeeping();
    if (restartRequested.exchange(false) && active) {
        deactivate();
        std::string error;
        if (!activate(sampleRate, error))
            log(kLogError, "Restart failed: " + error);
        latencyChanged = true;
    }
    if (latencyChanged.exchange(false)) {
        const uint32_t frames = active ? format->latency() : 0;
        shared->latency.store(frames);
        Writer w;
        w.u32(frames);
        send(Message::LatencyChanged, 0, w.data);
    }
    if (const uint32_t flags = paramsRescan.exchange(0)) {
        Writer w;
        w.u32(flags & 0x7fffffffu);
        send(Message::ParamsChanged, 0, w.data);
    }
    if (stateDirty.exchange(false))
        send(Message::StateDirty, 0);
    if (guiClosed.exchange(false)) {
        closeGui();
        send(Message::GuiClosed, 0);
    }
}

void sendLogs()
{
    std::vector<std::pair<int, std::string>> taken;
    {
        std::lock_guard<std::mutex> lock(newsMutex);
        taken.swap(logs);
    }
    for (const auto &entry : taken) {
        Writer w;
        w.u32(static_cast<uint32_t>(entry.first));
        w.str(entry.second);
        send(Message::Log, 0, w.data);
    }
}

HANDLE handleArg(const char *text) { return reinterpret_cast<HANDLE>(static_cast<uintptr_t>(std::strtoull(text, nullptr, 10))); }

}

int run(int argc, char **argv)
{
    // A crash ends the process at once: no error boxes, no debugger prompt
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    // Desktop pixels as the app counts them (it is per-monitor aware): no scaling between them
    using SetContext = BOOL(WINAPI *)(HANDLE);
    if (auto set = reinterpret_cast<SetContext>(reinterpret_cast<void *>(GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetProcessDpiAwarenessContext"))))
        set(reinterpret_cast<HANDLE>(static_cast<intptr_t>(-4))); // DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2
    if (argc != 6)
        return 2;
    controlIn = handleArg(argv[1]);
    controlOut = handleArg(argv[2]);
    HANDLE mapping = handleArg(argv[3]);
    processEvent = handleArg(argv[4]);
    doneEvent = handleArg(argv[5]);
    shared = static_cast<Shared *>(MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(Shared)));
    if (!shared || shared->magic != kMagic || shared->protocol != kProtocolVersion)
        return 2;

    mainThreadId = GetCurrentThreadId();
    messageEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    callbackEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    wakeAudio = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    audioAck = CreateEventW(nullptr, FALSE, FALSE, nullptr);

    std::thread(readerThread).detach();
    std::thread(audioThread).detach();

    HANDLE waits[2] = {messageEvent, callbackEvent};
    for (;;) {
        // The plugin's window (if it has one open) lives on this thread: its messages too
        MsgWaitForMultipleObjects(2, waits, FALSE, 10, QS_ALLINPUT);
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        std::deque<Incoming> batch;
        {
            std::lock_guard<std::mutex> lock(incomingMutex);
            batch.swap(incoming);
        }
        for (const Incoming &in : batch)
            handle(in);
        housekeeping();
        captureGui();
        sendLogs();
    }
}

}

int main(int argc, char **argv) { return host::run(argc, argv); }
