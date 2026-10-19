#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif

#include "Instance.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#include <algorithm>
#include <cstring>
#include <new>
#include <thread>

namespace plugin {

#ifdef _WIN32

namespace {

int64_t now()
{
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart;
}

// Reads exactly `size` bytes from a pipe, polling, until `deadline` (GetTickCount64). A host that
// stops halfway through a message can't hold the app here forever.
bool readExact(HANDLE pipe, void *data, DWORD size, ULONGLONG deadline)
{
    char *p = static_cast<char *>(data);
    int spins = 0;
    while (size > 0) {
        DWORD available = 0;
        if (!PeekNamedPipe(pipe, nullptr, 0, nullptr, &available, nullptr))
            return false;
        if (available == 0) {
            if (GetTickCount64() > deadline)
                return false;
            if (++spins < 200)
                SwitchToThread();
            else
                Sleep(1);
            continue;
        }
        DWORD got = 0;
        if (!ReadFile(pipe, p, std::min(size, available), &got, nullptr) || got == 0)
            return false;
        p += got;
        size -= got;
    }
    return true;
}

bool writeAll(HANDLE pipe, const void *data, DWORD size)
{
    const char *p = static_cast<const char *>(data);
    while (size > 0) {
        DWORD put = 0;
        if (!WriteFile(pipe, p, size, &put, nullptr) || put == 0)
            return false;
        p += put;
        size -= put;
    }
    return true;
}

// An inheritable copy of a handle, for one launch
HANDLE inheritable(HANDLE h)
{
    HANDLE copy = nullptr;
    DuplicateHandle(GetCurrentProcess(), h, GetCurrentProcess(), &copy, 0, TRUE, DUPLICATE_SAME_ACCESS);
    return copy;
}

void zero(const ProcessBlock &block)
{
    for (uint32_t c = 0; c < block.outputChannels; ++c)
        std::memset(block.outputs[c], 0, block.frames * sizeof(float));
}

}

Instance::~Instance()
{
    close();
    if (shared_) {
        shared_->~Shared();
        UnmapViewOfFile(shared_);
    }
    if (guiFrame_)
        UnmapViewOfFile(guiFrame_);
    for (void *h : {toHost_, fromHost_, mapping_, processEvent_, doneEvent_, timer_, guiMapping_})
        if (h)
            CloseHandle(static_cast<HANDLE>(h));
}

bool Instance::start(const SandboxOptions &options, std::string &error)
{
    LARGE_INTEGER frequency;
    QueryPerformanceFrequency(&frequency);
    ticksPerSecond_ = frequency.QuadPart;

    // Everything is made non-inheritable; inheritable copies exist only for the launch
    HANDLE hostIn = nullptr, appOut = nullptr, appIn = nullptr, hostOut = nullptr;
    if (!CreatePipe(&hostIn, &appOut, nullptr, 1 << 20) || !CreatePipe(&appIn, &hostOut, nullptr, 1 << 20)) {
        error = "Couldn't make the plugin's control pipes";
        return false;
    }
    toHost_ = appOut;
    fromHost_ = appIn;
    mapping_ = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, static_cast<DWORD>(sizeof(Shared)), nullptr);
    void *view = mapping_ ? MapViewOfFile(static_cast<HANDLE>(mapping_), FILE_MAP_ALL_ACCESS, 0, 0, sizeof(Shared)) : nullptr;
    processEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    doneEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    // The audio thread's deadline timer (high resolution where Windows has it, since 1803)
    timer_ = CreateWaitableTimerExW(nullptr, nullptr, 0x2 /* CREATE_WAITABLE_TIMER_HIGH_RESOLUTION */, TIMER_ALL_ACCESS);
    if (!timer_)
        timer_ = CreateWaitableTimerW(nullptr, FALSE, nullptr);
    if (!view || !processEvent_ || !doneEvent_) {
        if (view)
            UnmapViewOfFile(view);
        CloseHandle(hostIn);
        CloseHandle(hostOut);
        error = "Couldn't make the plugin's shared memory";
        return false;
    }
    shared_ = new (view) Shared();
    shared_->magic = kMagic;
    shared_->protocol = kProtocolVersion;

    HANDLE passed[5] = {inheritable(hostIn), inheritable(hostOut), inheritable(static_cast<HANDLE>(mapping_)),
                        inheritable(static_cast<HANDLE>(processEvent_)), inheritable(static_cast<HANDLE>(doneEvent_))};
    CloseHandle(hostIn);
    CloseHandle(hostOut);
    std::string arguments;
    std::vector<void *> inherit;
    for (HANDLE h : passed) {
        arguments += std::to_string(reinterpret_cast<uintptr_t>(h)) + " ";
        inherit.push_back(h);
    }
    const bool launched = process_.launch(hostExecutable(), arguments, inherit, options, error);
    for (HANDLE h : passed)
        if (h)
            CloseHandle(h);
    return launched;
}

std::unique_ptr<Instance> Instance::create(const Descriptor &plugin, const SandboxOptions &options, std::string &error)
{
    std::unique_ptr<Instance> instance(new Instance());
    instance->descriptor_ = plugin;
    SandboxOptions sandbox = options;
    sandbox.readable.push_back(plugin.path);
    if (!instance->start(sandbox, error))
        return nullptr;
    Writer w;
    w.str(plugin.path);
    w.str(plugin.id);
    std::string reply;
    if (!instance->call(Message::Load, w.data, reply, 15000, error))
        return nullptr;
    Reader r(reply);
    LoadedInfo &info = instance->info_;
    info.name = r.str();
    info.inputChannels = std::min(r.u32(), 64u);
    info.outputChannels = std::min(r.u32(), 64u);
    info.notePorts = std::min(r.u32(), 64u);
    info.params = r.u32() != 0;
    info.state = r.u32() != 0;
    const uint32_t gui = r.u32();
    info.gui = gui & 1;
    info.embedded = (gui & 2) != 0;
    if (!r.ok()) {
        error = "The plugin host answered nonsense";
        return nullptr;
    }
    return instance;
}

bool Instance::scan(const std::string &path, const SandboxOptions &options, std::vector<Descriptor> &found, std::string &error)
{
    Instance instance;
    SandboxOptions sandbox = options;
    sandbox.readable.push_back(path);
    if (!instance.start(sandbox, error))
        return false;
    Writer w;
    w.str(path);
    std::string reply;
    if (!instance.call(Message::Scan, w.data, reply, 10000, error))
        return false;
    Reader r(reply);
    const uint32_t count = std::min(r.u32(), 1000u);
    for (uint32_t i = 0; i < count && r.ok(); ++i) {
        Descriptor d = readDescriptor(r);
        d.path = path;
        if (r.ok() && !d.id.empty())
            found.push_back(std::move(d));
    }
    if (!r.ok()) {
        error = "The plugin host answered nonsense";
        return false;
    }
    return true;
}

bool Instance::alive() const { return status_.load() == Status::Ready && process_.running(); }

bool Instance::call(Message type, const std::string &payload, std::string &reply, uint32_t timeoutMs, std::string &error)
{
    std::lock_guard<std::mutex> lock(controlMutex_);
    if (!alive()) {
        error = "The plugin " + statusText();
        return false;
    }
    const uint32_t sequence = ++sequence_;
    if (!send(type, sequence, payload)) {
        error = "The plugin's host has gone";
        return false;
    }
    const ULONGLONG deadline = GetTickCount64() + timeoutMs;
    int spins = 0;
    for (;;) {
        bool replied = false, failed = false;
        if (!readMessages(sequence, replied, failed, reply)) {
            process_.wait(200);
            if (status_.load() == Status::Ready)
                status_ = process_.running() ? Status::Hung : Status::Crashed;
            exitCode_ = process_.exitCode();
            process_.terminate();
            error = "The plugin " + statusText();
            return false;
        }
        if (replied) {
            if (failed) {
                Reader r(reply);
                error = r.str();
                if (error.empty())
                    error = "The plugin refused";
                return false;
            }
            return true;
        }
        if (!process_.running()) {
            exitCode_ = process_.exitCode();
            status_ = Status::Crashed;
            active_ = false;
            error = "The plugin " + statusText();
            return false;
        }
        if (GetTickCount64() > deadline) {
            // Its main thread is stuck: it won't come back
            process_.terminate();
            status_ = Status::Hung;
            active_ = false;
            error = "The plugin " + statusText();
            return false;
        }
        if (++spins < 2000)
            SwitchToThread();
        else
            Sleep(1);
    }
}

bool Instance::readMessages(uint32_t sequence, bool &replied, bool &failed, std::string &reply)
{
    HANDLE pipe = static_cast<HANDLE>(fromHost_);
    for (;;) {
        DWORD available = 0;
        if (!PeekNamedPipe(pipe, nullptr, 0, nullptr, &available, nullptr))
            return false;
        if (available < sizeof(MessageHeader))
            return true;
        MessageHeader header{};
        const ULONGLONG deadline = GetTickCount64() + 2000;
        if (!readExact(pipe, &header, sizeof header, deadline) || header.size > kMaxMessageBytes)
            return false;
        std::string payload(header.size, '\0');
        if (header.size > 0 && !readExact(pipe, &payload[0], header.size, deadline + header.size / 10000))
            return false;
        const Message type = static_cast<Message>(header.type);
        if (type == Message::Reply || type == Message::Fail) {
            if (header.sequence == sequence && sequence != 0) {
                replied = true;
                failed = type == Message::Fail;
                reply = std::move(payload);
                return true;
            }
            continue; // the late reply of a request that gave up
        }
        handleNews(header, payload);
    }
}

void Instance::handleNews(const MessageHeader &header, const std::string &payload)
{
    Reader r(payload);
    switch (static_cast<Message>(header.type)) {
    case Message::Log: {
        const uint32_t severity = r.u32();
        std::string text = r.str();
        static const char *names[] = {"debug", "info", "warning", "error", "fatal", "host misbehaving", "plugin misbehaving"};
        text = std::string(severity < 7 ? names[severity] : "log") + ": " + text.substr(0, 1000);
        std::lock_guard<std::mutex> lock(newsMutex_);
        log_.push_back(std::move(text));
        while (log_.size() > 100)
            log_.pop_front();
        break;
    }
    case Message::ParamsChanged:
        paramsChanged_ = true;
        break;
    case Message::LatencyChanged:
        latency_ = std::min(r.u32(), 1u << 20);
        break;
    case Message::StateDirty:
        stateDirty_ = true;
        break;
    case Message::GuiClosed:
        guiOpen_ = false;
        guiEmbedded_ = false;
        break;
    default:
        break; // unknown news is ignored
    }
}

bool Instance::activate(double sampleRate, std::string &error)
{
    deactivate();
    Writer w;
    w.f64(sampleRate);
    std::string reply;
    if (!call(Message::Activate, w.data, reply, 10000, error))
        return false;
    Reader r(reply);
    latency_ = std::min(r.u32(), 1u << 20);
    sampleRate_ = sampleRate;
    // Nothing is on the audio side while inactive, so its state can be reset here
    request_ = shared_->done.load(std::memory_order_acquire);
    shared_->request.store(request_, std::memory_order_release);
    late_ = false;
    lateSince_ = 0;
    active_.store(true, std::memory_order_seq_cst);
    return true;
}

void Instance::deactivate()
{
    if (!active_.exchange(false, std::memory_order_seq_cst))
        return;
    // Wait for the audio thread to leave process(), then for the host to finish its last block
    while (inProcess_.load(std::memory_order_seq_cst))
        std::this_thread::yield();
    const ULONGLONG deadline = GetTickCount64() + 1000;
    while (shared_->done.load(std::memory_order_acquire) != request_ && alive() && GetTickCount64() < deadline)
        Sleep(1);
    if (shared_->done.load(std::memory_order_acquire) != request_ && alive()) {
        process_.terminate();
        status_ = Status::Hung;
        return;
    }
    std::string reply, error;
    call(Message::Deactivate, {}, reply, 5000, error);
}

bool Instance::params(std::vector<ParamInfo> &out, std::string &error)
{
    std::string reply;
    if (!call(Message::Params, {}, reply, 5000, error))
        return false;
    Reader r(reply);
    const uint32_t count = std::min(r.u32(), 10000u);
    out.clear();
    for (uint32_t i = 0; i < count && r.ok(); ++i) {
        ParamInfo p = readParam(r);
        // Sane ranges only: a plugin's numbers reach knobs and project files
        auto finite = [](double v) { return v == v && v > -1e12 && v < 1e12; };
        if (!finite(p.min) || !finite(p.max) || p.max < p.min)
            p.min = 0.0, p.max = 1.0;
        p.def = finite(p.def) ? std::clamp(p.def, p.min, p.max) : p.min;
        p.value = finite(p.value) ? std::clamp(p.value, p.min, p.max) : p.def;
        p.name = p.name.substr(0, 256);
        p.module = p.module.substr(0, 1024);
        out.push_back(std::move(p));
    }
    if (!r.ok()) {
        error = "The plugin host answered nonsense";
        return false;
    }
    return true;
}

bool Instance::paramText(uint32_t id, double value, std::string &text)
{
    Writer w;
    w.u32(id);
    w.f64(value);
    std::string reply, error;
    if (!call(Message::ParamText, w.data, reply, 1000, error))
        return false;
    Reader r(reply);
    text = r.str().substr(0, 64);
    return r.ok();
}

bool Instance::saveState(std::string &state, std::string &error, uint32_t timeoutMs)
{
    return call(Message::SaveState, {}, state, timeoutMs, error);
}

bool Instance::loadState(const std::string &state, std::string &error)
{
    std::string reply;
    return call(Message::LoadState, state, reply, 10000, error);
}

bool Instance::send(Message type, uint32_t sequence, const std::string &payload)
{
    std::lock_guard<std::mutex> lock(writeMutex_);
    const MessageHeader header{static_cast<uint32_t>(type), sequence, static_cast<uint32_t>(payload.size())};
    HANDLE pipe = static_cast<HANDLE>(toHost_);
    return writeAll(pipe, &header, sizeof header) && writeAll(pipe, payload.data(), static_cast<DWORD>(payload.size()));
}

bool Instance::openEmbeddedGui(uint32_t &width, uint32_t &height, std::string &error)
{
    if (!info_.embedded) {
        error = "This plugin's interface only opens as a window of its own";
        return false;
    }
    // The frame's memory, made once and handed to the host (its copy of the handle is its own)
    if (!guiMapping_) {
        guiMapping_ = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, static_cast<DWORD>(sizeof(GuiFrame)), nullptr);
        if (guiMapping_)
            guiFrame_ = static_cast<GuiFrame *>(MapViewOfFile(static_cast<HANDLE>(guiMapping_), FILE_MAP_ALL_ACCESS, 0, 0, sizeof(GuiFrame)));
        if (!guiFrame_) {
            error = "Couldn't make room for the plugin's interface";
            return false;
        }
    }
    HANDLE remote = nullptr;
    if (!DuplicateHandle(GetCurrentProcess(), static_cast<HANDLE>(guiMapping_), static_cast<HANDLE>(process_.handle()), &remote, 0, FALSE,
                         DUPLICATE_SAME_ACCESS)) {
        error = "Couldn't hand the plugin its interface's memory";
        return false;
    }
    Writer w;
    w.u32(static_cast<uint32_t>(reinterpret_cast<uintptr_t>(remote)));
    std::string reply;
    if (!call(Message::GuiEmbed, w.data, reply, 10000, error))
        return false;
    Reader r(reply);
    width = std::min(r.u32(), kGuiMaxWidth);
    height = std::min(r.u32(), kGuiMaxHeight);
    guiWindow_ = reinterpret_cast<void *>(static_cast<uintptr_t>(r.u32()));
    guiOpen_ = true;
    guiEmbedded_ = true;
    return true;
}

bool Instance::readGuiFrame(std::vector<uint8_t> &pixels, uint32_t &width, uint32_t &height, uint32_t &sequence)
{
    if (!guiFrame_ || !guiEmbedded_)
        return false;
    GuiFrame &f = *guiFrame_;
    const uint32_t before = f.sequence.load(std::memory_order_acquire);
    if (before == sequence || (before & 1))
        return false;
    const uint32_t w = std::min(f.width, kGuiMaxWidth), h = std::min(f.height, kGuiMaxHeight);
    pixels.resize(static_cast<size_t>(w) * h * 4);
    std::memcpy(pixels.data(), f.pixels, pixels.size());
    std::atomic_thread_fence(std::memory_order_acquire);
    if (f.sequence.load(std::memory_order_acquire) != before)
        return false; // written meanwhile: next time
    width = w;
    height = h;
    sequence = before;
    return w > 0 && h > 0;
}

void Instance::sendGuiInput(const GuiInput &in)
{
    if (!alive() || !guiEmbedded_)
        return;
    // A press may open a menu of the plugin's own, which needs to come to the front
    if (in.kind == GuiInputKind::Down || in.kind == GuiInputKind::DoubleClick)
        AllowSetForegroundWindow(hostProcessId());
    Writer w;
    w.u32(static_cast<uint32_t>(in.kind));
    w.u32(static_cast<uint32_t>(in.x));
    w.u32(static_cast<uint32_t>(in.y));
    w.u32(in.button);
    w.u32(in.mods);
    w.u32(static_cast<uint32_t>(in.wheel));
    w.u32(in.key);
    send(Message::GuiInput, 0, w.data);
}

void Instance::placeGui(int32_t x, int32_t y, bool shown, void *appWindow)
{
    HWND frame = static_cast<HWND>(guiWindow_);
    if (!alive() || !guiEmbedded_ || !frame || !IsWindow(frame))
        return;
    // Asynchronous: the request is posted to the host's thread, so a hung plugin can't hold the
    // app here
    const UINT flags = SWP_NOSIZE | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS;
    HWND app = static_cast<HWND>(appWindow);
    if (!shown || !app) {
        SetWindowPos(frame, HWND_BOTTOM, -24000, -24000, 0, 0, flags);
        return;
    }
    // Anywhere above the app's window is fine (it is there after a click in it brought it to the
    // front). Touching the stacking order, or even moving it to where it already is, makes a
    // plugin close its open menus (Surge XT's dropdowns closed a quarter second after opening),
    // so it is left alone unless it has moved or fallen below the app's window
    bool overApp = false;
    for (HWND w = GetWindow(app, GW_HWNDPREV); w; w = GetWindow(w, GW_HWNDPREV)) {
        if (w == frame) {
            overApp = true;
            break;
        }
    }
    RECT now{};
    const bool placed = GetWindowRect(frame, &now) && now.left == x && now.top == y;
    if (overApp && placed)
        return;
    if (overApp) {
        SetWindowPos(frame, nullptr, x, y, 0, 0, flags | SWP_NOZORDER);
        return;
    }
    // Just above the app's window: whatever was above that window stays above the frame
    HWND above = GetWindow(app, GW_HWNDPREV);
    while (above && !IsWindowVisible(above))
        above = GetWindow(above, GW_HWNDPREV);
    SetWindowPos(frame, above ? above : HWND_TOP, x, y, 0, 0, flags);
}

void Instance::resizeGui(uint32_t width, uint32_t height)
{
    if (!alive() || !guiEmbedded_)
        return;
    Writer w;
    w.u32(width);
    w.u32(height);
    send(Message::GuiResize, 0, w.data);
}

bool Instance::openGui(std::string &error)
{
    // A process in the background may not bring a window to the front unless allowed
    AllowSetForegroundWindow(hostProcessId());
    std::string reply;
    if (!call(Message::GuiOpen, {}, reply, 10000, error))
        return false;
    guiOpen_ = true;
    return true;
}

void Instance::closeGui()
{
    std::string reply, error;
    if (guiOpen_ && call(Message::GuiClose, {}, reply, 3000, error)) {
        guiOpen_ = false;
        guiEmbedded_ = false;
    }
}

void Instance::clipGui(int32_t x, int32_t y, int32_t width, int32_t height)
{
    if (!alive() || !guiEmbedded_)
        return;
    Writer w;
    w.u32(static_cast<uint32_t>(x));
    w.u32(static_cast<uint32_t>(y));
    w.u32(static_cast<uint32_t>(width));
    w.u32(static_cast<uint32_t>(height));
    send(Message::GuiClip, 0, w.data);
}

void Instance::allowToFront()
{
    if (alive())
        AllowSetForegroundWindow(hostProcessId());
}

uint32_t Instance::hostProcessId() const
{
    return process_.handle() ? GetProcessId(static_cast<HANDLE>(process_.handle())) : 0;
}

void Instance::close()
{
    if (!process_.running()) {
        if (status_.load() == Status::Ready)
            status_ = Status::Closed;
        return;
    }
    active_ = false;
    while (inProcess_.load(std::memory_order_seq_cst))
        std::this_thread::yield();
    {
        std::lock_guard<std::mutex> lock(controlMutex_);
        send(Message::Quit, ++sequence_, {});
    }
    if (!process_.wait(1000))
        process_.terminate();
    if (status_.load() == Status::Ready)
        status_ = Status::Closed;
}

bool Instance::setParam(uint32_t id, double value) { return shared_->toPlugin.push({ChangeKind::Value, id, value}); }

void Instance::poll()
{
    if (status_.load() == Status::Ready) {
        if (!process_.running()) {
            exitCode_ = process_.exitCode();
            active_ = false;
            status_ = Status::Crashed;
        } else {
            const int64_t since = lateSince_.load();
            const double limit = offline_.load() ? std::max(hangSeconds, 10.0) : hangSeconds;
            if (since != 0 && static_cast<double>(now() - since) / static_cast<double>(ticksPerSecond_) > limit) {
                process_.terminate();
                active_ = false;
                status_ = Status::Hung;
            }
        }
    }
    if (controlMutex_.try_lock()) {
        bool replied = false, failed = false;
        std::string reply;
        readMessages(0, replied, failed, reply);
        controlMutex_.unlock();
    }
    ParamChange change;
    std::lock_guard<std::mutex> lock(newsMutex_);
    while (shared_->fromPlugin.pop(change))
        if (changes_.size() < 4096 && change.value == change.value)
            changes_.push_back(change);
}

std::vector<ParamChange> Instance::takeChanges()
{
    std::lock_guard<std::mutex> lock(newsMutex_);
    std::vector<ParamChange> out;
    out.swap(changes_);
    return out;
}

std::vector<std::string> Instance::takeLog()
{
    std::lock_guard<std::mutex> lock(newsMutex_);
    std::vector<std::string> out(log_.begin(), log_.end());
    log_.clear();
    return out;
}

std::string Instance::statusText() const
{
    switch (status_.load()) {
    case Status::Ready:
        return process_.contained() ? "is running in its sandbox" : "is running (not sandboxed)";
    case Status::Crashed:
        return "crashed (" + describeExit(exitCode_.load()) + ")";
    case Status::Hung:
        return "stopped responding, so it was stopped";
    case Status::Closed:
        return "is closed";
    }
    return {};
}

uint32_t Instance::latency() const { return latency_.load(); }

bool Instance::process(const ProcessBlock &block)
{
    inProcess_.store(true, std::memory_order_seq_cst);
    if (!active_.load(std::memory_order_seq_cst) || status_.load(std::memory_order_acquire) != Status::Ready || block.frames > kMaxFrames) {
        inProcess_.store(false, std::memory_order_release);
        zero(block);
        return false;
    }
    Shared &s = *shared_;
    // Still waiting for a late block: don't wait again until the host has caught up
    if (late_) {
        if (s.done.load(std::memory_order_acquire) != request_) {
            missed_.fetch_add(1, std::memory_order_relaxed);
            inProcess_.store(false, std::memory_order_release);
            zero(block);
            return false;
        }
        late_ = false;
        lateSince_.store(0);
    }

    // The request
    const uint32_t inputs = std::min(block.inputChannels, kMaxChannels);
    for (uint32_t c = 0; c < inputs; ++c)
        std::memcpy(s.input[c], block.inputs[c], block.frames * sizeof(float));
    s.inputChannels = inputs;
    s.frames = block.frames;
    s.transport = block.transport;
    const uint32_t events = std::min(block.eventCount, kMaxBlockEvents);
    if (events > 0)
        std::memcpy(s.events, block.events, events * sizeof(Event));
    s.eventCount = events;
    ++request_;
    s.request.store(request_, std::memory_order_release);
    SetEvent(static_cast<HANDLE>(processEvent_));

    // The wait: a short spin (most plugins answer in microseconds), then the kernel until the
    // answer, the host's end or the deadline, timed by a high-resolution timer since a plain
    // wait's timeout is rounded up to the system tick (15.6 ms). Waiting takes no lock.
    const double budget = offline_.load(std::memory_order_relaxed) ? 5.0 : std::max(0.002, 0.85 * block.frames / sampleRate_);
    const int64_t start = now();
    const int64_t deadline = start + static_cast<int64_t>(budget * static_cast<double>(ticksPerSecond_));
    const int64_t spinUntil = start + ticksPerSecond_ / 20000; // 50 microseconds
    while (s.done.load(std::memory_order_acquire) != request_ && now() < spinUntil)
        YieldProcessor();
    if (s.done.load(std::memory_order_acquire) != request_) {
        HANDLE waits[3] = {static_cast<HANDLE>(doneEvent_), static_cast<HANDLE>(process_.handle()), static_cast<HANDLE>(timer_)};
        const DWORD count = timer_ ? 3 : 2;
        if (timer_) {
            LARGE_INTEGER due;
            due.QuadPart = -std::max<int64_t>(1, (deadline - now()) * 10000000 / ticksPerSecond_); // relative, 100 ns units
            SetWaitableTimer(static_cast<HANDLE>(timer_), &due, 0, nullptr, nullptr, FALSE);
        }
        while (s.done.load(std::memory_order_acquire) != request_) {
            const int64_t left = deadline - now();
            if (left <= 0)
                break;
            const DWORD ms = static_cast<DWORD>(std::max<int64_t>(1, (left * 1000 + ticksPerSecond_ - 1) / ticksPerSecond_));
            const DWORD woke = WaitForMultipleObjects(count, waits, FALSE, timer_ ? ms + 20 : ms);
            if (woke == WAIT_OBJECT_0 + 1 || woke == WAIT_OBJECT_0 + 2 || woke == WAIT_FAILED)
                break; // the host ended, or the deadline came
        }
        if (timer_)
            CancelWaitableTimer(static_cast<HANDLE>(timer_));
    }
    if (s.done.load(std::memory_order_acquire) != request_) {
        late_ = true;
        int64_t expected = 0;
        lateSince_.compare_exchange_strong(expected, start);
        missed_.fetch_add(1, std::memory_order_relaxed);
        inProcess_.store(false, std::memory_order_release);
        zero(block);
        return false;
    }

    // The answer, checked: no NaNs or wild values reach the mix
    const bool ok = s.status != 0; // CLAP_PROCESS_ERROR
    for (uint32_t c = 0; c < block.outputChannels; ++c) {
        const float *from = s.output[std::min(c, kMaxChannels - 1)];
        float *to = block.outputs[c];
        for (uint32_t i = 0; i < block.frames; ++i) {
            const float v = from[i];
            to[i] = v > -16.0f && v < 16.0f ? v : (v >= 16.0f ? 16.0f : (v <= -16.0f ? -16.0f : 0.0f));
        }
    }
    inProcess_.store(false, std::memory_order_release);
    return ok;
}

#else

Instance::~Instance() {}
std::unique_ptr<Instance> Instance::create(const Descriptor &, const SandboxOptions &, std::string &error)
{
    error = "Plugins are only hosted on Windows for now";
    return nullptr;
}
bool Instance::scan(const std::string &, const SandboxOptions &, std::vector<Descriptor> &, std::string &error)
{
    error = "Plugins are only hosted on Windows for now";
    return false;
}
bool Instance::activate(double, std::string &error)
{
    error = "Unsupported";
    return false;
}
void Instance::deactivate() {}
bool Instance::params(std::vector<ParamInfo> &, std::string &) { return false; }
bool Instance::paramText(uint32_t, double, std::string &) { return false; }
bool Instance::saveState(std::string &, std::string &, uint32_t) { return false; }
bool Instance::loadState(const std::string &, std::string &) { return false; }
bool Instance::openGui(std::string &error)
{
    error = "Unsupported";
    return false;
}
void Instance::closeGui() {}
bool Instance::openEmbeddedGui(uint32_t &, uint32_t &, std::string &error)
{
    error = "Unsupported";
    return false;
}
bool Instance::readGuiFrame(std::vector<uint8_t> &, uint32_t &, uint32_t &, uint32_t &) { return false; }
void Instance::sendGuiInput(const GuiInput &) {}
void Instance::resizeGui(uint32_t, uint32_t) {}
void Instance::placeGui(int32_t, int32_t, bool, void *) {}
void Instance::allowToFront() {}
void Instance::clipGui(int32_t, int32_t, int32_t, int32_t) {}
bool Instance::send(Message, uint32_t, const std::string &) { return false; }
uint32_t Instance::hostProcessId() const { return 0; }
void Instance::close() {}
bool Instance::setParam(uint32_t, double) { return false; }
void Instance::poll() {}
std::vector<ParamChange> Instance::takeChanges() { return {}; }
std::vector<std::string> Instance::takeLog() { return {}; }
std::string Instance::statusText() const { return "is unsupported here"; }
uint32_t Instance::latency() const { return 0; }
bool Instance::process(const ProcessBlock &block)
{
    for (uint32_t c = 0; c < block.outputChannels; ++c)
        std::fill(block.outputs[c], block.outputs[c] + block.frames, 0.0f);
    return false;
}

#endif

}
