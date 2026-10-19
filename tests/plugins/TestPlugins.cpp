// vibin-test-plugins.clap: the CLAP plugins the plugin self-test loads through the sandbox. Plain
// CLAP, no helpers, so it also shows the least a plugin has to do.
//
// - vibin.test.gain: a stereo gain effect. Parameters: Gain (dB, -60 to +12), Mute (stepped) and
//   Peak (read-only: the plugin reports each block's output peak as a parameter change, which is
//   how the self-test sees changes coming back from a plugin). Saves Gain and Mute as its state.
//   Latency (0-4096 frames, stepped) delays its sound by that much and reports it through the
//   latency extension, asking the host to restart it when it changes (CLAP's way: latency only
//   changes while inactive), for the delay compensation checks.
//   It has a window of its own (CLAP's floating win32 interface): clicking its left half takes
//   1 dB off, its right half adds 1 dB, sent to the host as a gesture, as a real editor would.
// - vibin.test.sine: an instrument: sine voices for CLAP or MIDI notes, Level parameter.
// - vibin.test.faulty: a stereo pass-through whose Fault parameter makes it misbehave on the next
//   block: crash (a write through a null pointer), hang (spins forever) or slow (sleeps 60 ms a
//   block). When it is made it tries to write a file in the user's profile and logs whether it
//   could, so the self-test sees the sandbox stop it.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <clap/clap.h>

namespace {

constexpr double kPi = 3.14159265358979323846;
std::string pluginFolder; // from the entry's init, for the probe beside the plugin's own file

const clap_host_log *hostLog(const clap_host *host)
{
    return static_cast<const clap_host_log *>(host->get_extension(host, CLAP_EXT_LOG));
}

void logTo(const clap_host *host, clap_log_severity severity, const char *message)
{
    if (const clap_host_log *log = hostLog(host))
        log->log(host, severity, message);
}

// The parts every plugin here shares: the clap_plugin with this as its data, stereo audio ports
struct Base {
    clap_plugin plugin{};
    const clap_host *host = nullptr;
    double sampleRate = 48000.0;
    bool hasInput = true;

    virtual ~Base() = default;
    virtual bool init() { return true; }
    virtual void activate() {}
    virtual clap_process_status process(const clap_process *process) = 0;
    virtual const void *extension(const char *) { return nullptr; }
    // Parameters
    virtual uint32_t paramCount() { return 0; }
    virtual bool paramInfo(uint32_t, clap_param_info *) { return false; }
    virtual bool paramValue(clap_id, double *) { return false; }
    virtual bool paramText(clap_id, double value, char *out, uint32_t size)
    {
        std::snprintf(out, size, "%.2f", value);
        return true;
    }
    virtual void setParam(clap_id, double) {}

    // Applies the parameter changes among a block's events (or a flush's)
    void readEvents(const clap_input_events *in)
    {
        const uint32_t n = in->size(in);
        for (uint32_t i = 0; i < n; ++i) {
            const clap_event_header *e = in->get(in, i);
            if (e->space_id == CLAP_CORE_EVENT_SPACE_ID && e->type == CLAP_EVENT_PARAM_VALUE) {
                const auto *p = reinterpret_cast<const clap_event_param_value *>(e);
                setParam(p->param_id, p->value);
            }
        }
    }
};

Base *self(const clap_plugin *p) { return static_cast<Base *>(p->plugin_data); }

// The extensions, forwarding to Base
const clap_plugin_audio_ports kAudioPorts = {
    [](const clap_plugin *p, bool isInput) -> uint32_t { return isInput && !self(p)->hasInput ? 0u : 1u; },
    [](const clap_plugin *p, uint32_t index, bool isInput, clap_audio_port_info *info) -> bool {
        if (index != 0 || (isInput && !self(p)->hasInput))
            return false;
        info->id = isInput ? 0 : 1;
        std::snprintf(info->name, sizeof info->name, "%s", isInput ? "In" : "Out");
        info->flags = CLAP_AUDIO_PORT_IS_MAIN;
        info->channel_count = 2;
        info->port_type = CLAP_PORT_STEREO;
        info->in_place_pair = CLAP_INVALID_ID;
        return true;
    },
};

const clap_plugin_params kParams = {
    [](const clap_plugin *p) { return self(p)->paramCount(); },
    [](const clap_plugin *p, uint32_t index, clap_param_info *info) { return self(p)->paramInfo(index, info); },
    [](const clap_plugin *p, clap_id id, double *value) { return self(p)->paramValue(id, value); },
    [](const clap_plugin *p, clap_id id, double value, char *out, uint32_t size) { return self(p)->paramText(id, value, out, size); },
    [](const clap_plugin *, clap_id, const char *text, double *value) {
        *value = std::atof(text);
        return true;
    },
    [](const clap_plugin *p, const clap_input_events *in, const clap_output_events *) { self(p)->readEvents(in); },
};

void fillParam(clap_param_info *info, clap_id id, const char *name, double min, double max, double def, clap_param_info_flags flags)
{
    std::memset(info, 0, sizeof *info);
    info->id = id;
    info->flags = flags;
    std::snprintf(info->name, sizeof info->name, "%s", name);
    info->min_value = min;
    info->max_value = max;
    info->default_value = def;
}

// --- Gain

struct Gain : Base {
    std::atomic<double> gainDb{0.0};
    std::atomic<double> mute{0.0};
    double peak = 0.0;
    HWND window = nullptr;
    std::atomic<bool> edited{false}; // the window changed Gain: tell the host at the next block
    std::atomic<double> wantedLatency{0.0}; // the Latency parameter; becomes `latency` on activation
    uint32_t latency = 0;
    std::vector<float> delayLine[2];
    uint32_t delayAt = 0;

    void activate() override
    {
        latency = static_cast<uint32_t>(wantedLatency.load());
        for (auto &line : delayLine)
            line.assign(8192, 0.0f);
        delayAt = 0;
    }

    static LRESULT CALLBACK windowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
    {
        Gain *g = reinterpret_cast<Gain *>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        switch (message) {
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(hwnd, &ps);
            char text[64];
            std::snprintf(text, sizeof text, "Gain %.1f dB", g ? g->gainDb.load() : 0.0);
            TextOutA(dc, 16, 16, text, static_cast<int>(std::strlen(text)));
            const char *hint = "Click the left half for -1 dB, the right half for +1 dB";
            TextOutA(dc, 16, 44, hint, static_cast<int>(std::strlen(hint)));
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_LBUTTONDOWN:
            if (g) {
                RECT r;
                GetClientRect(hwnd, &r);
                const double step = static_cast<short>(LOWORD(lParam)) < (r.right - r.left) / 2 ? -1.0 : 1.0;
                g->setParam(1, g->gainDb.load() + step);
                g->edited = true;
                InvalidateRect(hwnd, nullptr, TRUE);
            }
            return 0;
        case WM_CLOSE:
            // Closing the window hides it; the host is told, and destroys it
            ShowWindow(hwnd, SW_HIDE);
            if (g)
                if (auto *gui = static_cast<const clap_host_gui *>(g->host->get_extension(g->host, CLAP_EXT_GUI)))
                    gui->closed(g->host, false);
            return 0;
        }
        return DefWindowProcW(hwnd, message, wParam, lParam);
    }

    bool floating = true;

    bool createWindow(HWND parent = nullptr)
    {
        static bool registered = false;
        HINSTANCE instance = GetModuleHandleW(nullptr);
        if (!registered) {
            WNDCLASSW wc{};
            wc.lpfnWndProc = windowProc;
            wc.hInstance = instance;
            wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
            wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
            wc.lpszClassName = L"VibinTestGainEditor";
            registered = RegisterClassW(&wc) != 0;
        }
        if (parent)
            window = CreateWindowExW(0, L"VibinTestGainEditor", L"", WS_CHILD | WS_VISIBLE, 0, 0, 420, 140, parent, nullptr, instance, nullptr);
        else
            window = CreateWindowExW(0, L"VibinTestGainEditor", L"Test Gain", WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, CW_USEDEFAULT,
                                     CW_USEDEFAULT, 420, 140, nullptr, nullptr, instance, nullptr);
        if (window)
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
        return window != nullptr;
    }

    uint32_t paramCount() override { return 4; }
    bool paramInfo(uint32_t index, clap_param_info *info) override
    {
        if (index == 0)
            fillParam(info, 1, "Gain", -60.0, 12.0, 0.0, CLAP_PARAM_IS_AUTOMATABLE);
        else if (index == 1)
            fillParam(info, 2, "Mute", 0.0, 1.0, 0.0, CLAP_PARAM_IS_STEPPED | CLAP_PARAM_IS_AUTOMATABLE);
        else if (index == 2)
            fillParam(info, 3, "Peak", 0.0, 4.0, 0.0, CLAP_PARAM_IS_READONLY);
        else if (index == 3)
            fillParam(info, 4, "Latency", 0.0, 4096.0, 0.0, CLAP_PARAM_IS_STEPPED);
        else
            return false;
        return true;
    }
    bool paramValue(clap_id id, double *value) override
    {
        if (id == 1)
            *value = gainDb;
        else if (id == 2)
            *value = mute;
        else if (id == 3)
            *value = peak;
        else if (id == 4)
            *value = wantedLatency;
        else
            return false;
        return true;
    }
    bool paramText(clap_id id, double value, char *out, uint32_t size) override
    {
        if (id == 1)
            std::snprintf(out, size, "%.1f dB", value);
        else if (id == 2)
            std::snprintf(out, size, "%s", value >= 0.5 ? "Muted" : "On");
        else if (id == 4)
            std::snprintf(out, size, "%.0f samples", value);
        else
            std::snprintf(out, size, "%.3f", value);
        return true;
    }
    void setParam(clap_id id, double value) override
    {
        if (id == 1)
            gainDb = std::fmin(12.0, std::fmax(-60.0, value));
        else if (id == 2)
            mute = value >= 0.5 ? 1.0 : 0.0;
        else if (id == 4) {
            const double frames = std::round(std::fmin(4096.0, std::fmax(0.0, value)));
            if (frames != wantedLatency.exchange(frames))
                host->request_restart(host); // the new latency takes effect on reactivation
        }
    }

    clap_process_status process(const clap_process *process) override
    {
        readEvents(process->in_events);
        const float g = mute >= 0.5 ? 0.0f : static_cast<float>(std::pow(10.0, gainDb / 20.0));
        float top = 0.0f;
        for (uint32_t c = 0; c < 2; ++c) {
            const float *in = process->audio_inputs[0].data32[c];
            float *out = process->audio_outputs[0].data32[c];
            float *line = delayLine[c].data();
            for (uint32_t i = 0; i < process->frames_count; ++i) {
                const uint32_t at = (delayAt + i) & 8191;
                line[at] = in[i] * g;
                out[i] = line[(at - latency) & 8191];
                top = std::fmax(top, std::fabs(out[i]));
            }
        }
        delayAt = (delayAt + process->frames_count) & 8191;
        peak = top;
        clap_event_param_value e{};
        e.header.size = sizeof e;
        e.header.time = 0;
        e.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
        e.header.type = CLAP_EVENT_PARAM_VALUE;
        e.param_id = 3;
        e.note_id = -1;
        e.port_index = -1;
        e.channel = -1;
        e.key = -1;
        e.value = top;
        process->out_events->try_push(process->out_events, &e.header);
        if (edited.exchange(false)) {
            // A change from the window, as a gesture
            clap_event_param_gesture gesture{};
            gesture.header = {sizeof gesture, 0, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_PARAM_GESTURE_BEGIN, 0};
            gesture.param_id = 1;
            process->out_events->try_push(process->out_events, &gesture.header);
            e.param_id = 1;
            e.value = gainDb;
            process->out_events->try_push(process->out_events, &e.header);
            gesture.header.type = CLAP_EVENT_PARAM_GESTURE_END;
            process->out_events->try_push(process->out_events, &gesture.header);
        }
        return CLAP_PROCESS_CONTINUE;
    }

    // State: "gain=<dB> mute=<0|1>"
    static bool save(const clap_plugin *p, const clap_ostream *stream)
    {
        const Gain *g = static_cast<const Gain *>(self(p));
        char text[64];
        const int n = std::snprintf(text, sizeof text, "gain=%.6f mute=%d", g->gainDb.load(), g->mute.load() >= 0.5 ? 1 : 0);
        return stream->write(stream, text, static_cast<uint64_t>(n)) == n;
    }
    static bool load(const clap_plugin *p, const clap_istream *stream)
    {
        Gain *g = static_cast<Gain *>(self(p));
        char text[64] = {};
        int64_t got = 0;
        while (got < static_cast<int64_t>(sizeof text) - 1) {
            const int64_t n = stream->read(stream, text + got, static_cast<uint64_t>(sizeof text - 1 - got));
            if (n <= 0)
                break;
            got += n;
        }
        double db = 0.0;
        int muted = 0;
        if (std::sscanf(text, "gain=%lf mute=%d", &db, &muted) != 2)
            return false;
        g->setParam(1, db);
        g->setParam(2, muted);
        return true;
    }

    const void *extension(const char *id) override
    {
        static const clap_plugin_state state = {save, load};
        static const clap_plugin_gui gui = {
            [](const clap_plugin *, const char *api, bool) { return !std::strcmp(api, CLAP_WINDOW_API_WIN32); },
            [](const clap_plugin *, const char **api, bool *floating) {
                *api = CLAP_WINDOW_API_WIN32;
                *floating = true;
                return true;
            },
            [](const clap_plugin *p, const char *api, bool floating) {
                Gain *g = static_cast<Gain *>(self(p));
                if (std::strcmp(api, CLAP_WINDOW_API_WIN32) != 0)
                    return false;
                g->floating = floating;
                // Embedded: its window is made when the host gives it a parent
                return floating ? g->createWindow() : true;
            },
            [](const clap_plugin *p) {
                Gain *g = static_cast<Gain *>(self(p));
                if (g->window)
                    DestroyWindow(g->window);
                g->window = nullptr;
            },
            [](const clap_plugin *, double) { return false; },
            [](const clap_plugin *, uint32_t *w, uint32_t *h) {
                *w = 420;
                *h = 140;
                return true;
            },
            [](const clap_plugin *) { return false; },
            [](const clap_plugin *, clap_gui_resize_hints *) { return false; },
            [](const clap_plugin *, uint32_t *, uint32_t *) { return false; },
            [](const clap_plugin *, uint32_t, uint32_t) { return false; },
            [](const clap_plugin *p, const clap_window *parent) {
                Gain *g = static_cast<Gain *>(self(p));
                return !g->floating && !g->window && parent && g->createWindow(static_cast<HWND>(parent->win32));
            },
            [](const clap_plugin *, const clap_window *) { return true; },
            [](const clap_plugin *p, const char *title) {
                if (HWND w = static_cast<Gain *>(self(p))->window)
                    SetWindowTextA(w, title);
            },
            [](const clap_plugin *p) {
                HWND w = static_cast<Gain *>(self(p))->window;
                if (!w)
                    return false;
                ShowWindow(w, SW_SHOWNORMAL);
                if (static_cast<Gain *>(self(p))->floating)
                    SetForegroundWindow(w);
                return true;
            },
            [](const clap_plugin *p) {
                if (HWND w = static_cast<Gain *>(self(p))->window)
                    ShowWindow(w, SW_HIDE);
                return true;
            },
        };
        if (!std::strcmp(id, CLAP_EXT_STATE))
            return &state;
        static const clap_plugin_latency latencyExt = {[](const clap_plugin *p) { return static_cast<Gain *>(self(p))->latency; }};
        if (!std::strcmp(id, CLAP_EXT_GUI))
            return &gui;
        if (!std::strcmp(id, CLAP_EXT_LATENCY))
            return &latencyExt;
        return nullptr;
    }
};

// --- Sine

struct Sine : Base {
    struct Voice {
        int key = -1;
        double phase = 0.0;
        double step = 0.0;
        float level = 0.0f;
        float release = 1.0f; // 1 while held, falling to 0 once released
        bool held = false;
    };
    Voice voices[16];
    std::atomic<double> level{0.5};

    Sine() { hasInput = false; }

    uint32_t paramCount() override { return 1; }
    bool paramInfo(uint32_t index, clap_param_info *info) override
    {
        if (index != 0)
            return false;
        fillParam(info, 1, "Level", 0.0, 1.0, 0.5, CLAP_PARAM_IS_AUTOMATABLE);
        return true;
    }
    bool paramValue(clap_id id, double *value) override
    {
        if (id != 1)
            return false;
        *value = level;
        return true;
    }
    void setParam(clap_id id, double value) override
    {
        if (id == 1)
            level = std::fmin(1.0, std::fmax(0.0, value));
    }

    void noteOn(int key, double velocity)
    {
        Voice *v = &voices[0];
        for (Voice &candidate : voices)
            if (candidate.key < 0 || candidate.release <= 0.0f) {
                v = &candidate;
                break;
            }
        v->key = key;
        v->phase = 0.0;
        v->step = 2.0 * kPi * 440.0 * std::pow(2.0, (key - 69) / 12.0) / sampleRate;
        v->level = static_cast<float>(velocity);
        v->release = 1.0f;
        v->held = true;
    }
    void noteOff(int key)
    {
        for (Voice &v : voices)
            if (v.key == key)
                v.held = false;
    }

    void event(const clap_event_header *e)
    {
        if (e->space_id != CLAP_CORE_EVENT_SPACE_ID)
            return;
        if (e->type == CLAP_EVENT_NOTE_ON || e->type == CLAP_EVENT_NOTE_OFF) {
            const auto *n = reinterpret_cast<const clap_event_note *>(e);
            // A wildcard note off (key -1) is ignored, as Surge XT does: the host has to release
            // its notes by key
            if (e->type == CLAP_EVENT_NOTE_ON)
                noteOn(n->key, n->velocity);
            else if (n->key >= 0)
                noteOff(n->key);
        } else if (e->type == CLAP_EVENT_MIDI) {
            const auto *m = reinterpret_cast<const clap_event_midi *>(e);
            const int status = m->data[0] & 0xF0;
            if (status == 0x90 && m->data[2] > 0)
                noteOn(m->data[1], m->data[2] / 127.0);
            else if (status == 0x80 || status == 0x90)
                noteOff(m->data[1]);
        } else if (e->type == CLAP_EVENT_PARAM_VALUE) {
            const auto *p = reinterpret_cast<const clap_event_param_value *>(e);
            setParam(p->param_id, p->value);
        }
    }

    clap_process_status process(const clap_process *process) override
    {
        float *left = process->audio_outputs[0].data32[0];
        float *right = process->audio_outputs[0].data32[1];
        const uint32_t frames = process->frames_count;
        const clap_input_events *in = process->in_events;
        const uint32_t count = in->size(in);
        uint32_t next = 0;
        const float releaseStep = static_cast<float>(1.0 / (0.01 * sampleRate));
        for (uint32_t i = 0; i < frames; ++i) {
            while (next < count && in->get(in, next)->time <= i)
                event(in->get(in, next++));
            float sum = 0.0f;
            for (Voice &v : voices) {
                if (v.key < 0)
                    continue;
                if (!v.held) {
                    v.release -= releaseStep;
                    if (v.release <= 0.0f) {
                        v.key = -1;
                        continue;
                    }
                }
                sum += static_cast<float>(std::sin(v.phase)) * v.level * v.release;
                v.phase += v.step;
                if (v.phase > 2.0 * kPi)
                    v.phase -= 2.0 * kPi;
            }
            left[i] = right[i] = sum * static_cast<float>(level.load(std::memory_order_relaxed));
        }
        while (next < count)
            event(in->get(in, next++));
        return CLAP_PROCESS_CONTINUE;
    }

    const void *extension(const char *id) override
    {
        static const clap_plugin_note_ports notePorts = {
            [](const clap_plugin *, bool isInput) -> uint32_t { return isInput ? 1u : 0u; },
            [](const clap_plugin *, uint32_t index, bool isInput, clap_note_port_info *info) -> bool {
                if (index != 0 || !isInput)
                    return false;
                info->id = 0;
                info->supported_dialects = CLAP_NOTE_DIALECT_CLAP | CLAP_NOTE_DIALECT_MIDI;
                info->preferred_dialect = CLAP_NOTE_DIALECT_CLAP;
                std::snprintf(info->name, sizeof info->name, "Notes");
                return true;
            },
        };
        if (!std::strcmp(id, CLAP_EXT_NOTE_PORTS))
            return &notePorts;
        return nullptr;
    }
};

// --- Faulty

struct Faulty : Base {
    std::atomic<double> fault{0.0};

    bool init() override
    {
        // The sandbox check: can this plugin write into the user's profile?
        char profile[MAX_PATH] = {};
        if (GetEnvironmentVariableA("USERPROFILE", profile, sizeof profile) == 0) {
            logTo(host, CLAP_LOG_INFO, "probe: no profile");
            return true;
        }
        // And beside its own file, which a folder granted for writing allows
        if (!pluginFolder.empty()) {
            const std::string own = pluginFolder + "\\vibin-sandbox-probe.tmp";
            HANDLE beside = CreateFileA(own.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY, nullptr);
            if (beside == INVALID_HANDLE_VALUE) {
                logTo(host, CLAP_LOG_INFO, "probe-own-folder: write denied");
            } else {
                CloseHandle(beside);
                DeleteFileA(own.c_str());
                logTo(host, CLAP_LOG_INFO, "probe-own-folder: write allowed");
            }
        }
        const std::string path = std::string(profile) + "\\vibin-sandbox-probe.tmp";
        HANDLE file = CreateFileA(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY, nullptr);
        if (file == INVALID_HANDLE_VALUE) {
            logTo(host, CLAP_LOG_INFO, "probe: write denied");
        } else {
            CloseHandle(file);
            DeleteFileA(path.c_str());
            logTo(host, CLAP_LOG_INFO, "probe: write allowed");
        }
        return true;
    }

    uint32_t paramCount() override { return 1; }
    bool paramInfo(uint32_t index, clap_param_info *info) override
    {
        if (index != 0)
            return false;
        fillParam(info, 1, "Fault", 0.0, 3.0, 0.0, CLAP_PARAM_IS_STEPPED | CLAP_PARAM_IS_ENUM);
        return true;
    }
    bool paramValue(clap_id id, double *value) override
    {
        if (id != 1)
            return false;
        *value = fault;
        return true;
    }
    bool paramText(clap_id, double value, char *out, uint32_t size) override
    {
        static const char *names[] = {"None", "Crash", "Hang", "Slow"};
        const int i = static_cast<int>(std::lround(std::fmin(3.0, std::fmax(0.0, value))));
        std::snprintf(out, size, "%s", names[i]);
        return true;
    }
    void setParam(clap_id id, double value) override
    {
        if (id == 1)
            fault = value;
    }

    clap_process_status process(const clap_process *process) override
    {
        readEvents(process->in_events);
        const int mode = static_cast<int>(std::lround(fault.load()));
        if (mode == 1) {
            volatile int *nowhere = nullptr;
            *nowhere = 1;
        } else if (mode == 2) {
            volatile bool forever = true;
            while (forever) {
            }
        } else if (mode == 3) {
            Sleep(60);
        }
        for (uint32_t c = 0; c < 2; ++c)
            std::memcpy(process->audio_outputs[0].data32[c], process->audio_inputs[0].data32[c], process->frames_count * sizeof(float));
        return CLAP_PROCESS_CONTINUE;
    }
};

// --- The plugin table

const char *const kEffectFeatures[] = {CLAP_PLUGIN_FEATURE_AUDIO_EFFECT, CLAP_PLUGIN_FEATURE_UTILITY, CLAP_PLUGIN_FEATURE_STEREO, nullptr};
const char *const kInstrumentFeatures[] = {CLAP_PLUGIN_FEATURE_INSTRUMENT, CLAP_PLUGIN_FEATURE_SYNTHESIZER, CLAP_PLUGIN_FEATURE_STEREO, nullptr};

const clap_plugin_descriptor kDescriptors[] = {
    {CLAP_VERSION_INIT, "vibin.test.gain", "Test Gain", "Vibin", "", "", "", "1.0.0", "A gain for the plugin self-test", kEffectFeatures},
    {CLAP_VERSION_INIT, "vibin.test.sine", "Test Sine", "Vibin", "", "", "", "1.0.0", "Sine voices for the plugin self-test", kInstrumentFeatures},
    {CLAP_VERSION_INIT, "vibin.test.faulty", "Test Faulty", "Vibin", "", "", "", "1.0.0", "Crashes, hangs or stalls on request", kEffectFeatures},
};

const clap_plugin *create(const clap_host *host, const char *id)
{
    Base *b = nullptr;
    const clap_plugin_descriptor *desc = nullptr;
    if (!std::strcmp(id, kDescriptors[0].id)) {
        b = new Gain;
        desc = &kDescriptors[0];
    } else if (!std::strcmp(id, kDescriptors[1].id)) {
        b = new Sine;
        desc = &kDescriptors[1];
    } else if (!std::strcmp(id, kDescriptors[2].id)) {
        b = new Faulty;
        desc = &kDescriptors[2];
    } else {
        return nullptr;
    }
    b->host = host;
    clap_plugin &p = b->plugin;
    p.desc = desc;
    p.plugin_data = b;
    p.init = [](const clap_plugin *p) { return self(p)->init(); };
    p.destroy = [](const clap_plugin *p) { delete self(p); };
    p.activate = [](const clap_plugin *p, double rate, uint32_t, uint32_t) {
        self(p)->sampleRate = rate;
        self(p)->activate();
        return true;
    };
    p.deactivate = [](const clap_plugin *) {};
    p.start_processing = [](const clap_plugin *) { return true; };
    p.stop_processing = [](const clap_plugin *) {};
    p.reset = [](const clap_plugin *) {};
    p.process = [](const clap_plugin *p, const clap_process *process) { return self(p)->process(process); };
    p.get_extension = [](const clap_plugin *p, const char *id) -> const void * {
        if (!std::strcmp(id, CLAP_EXT_AUDIO_PORTS))
            return &kAudioPorts;
        if (!std::strcmp(id, CLAP_EXT_PARAMS))
            return &kParams;
        return self(p)->extension(id);
    };
    p.on_main_thread = [](const clap_plugin *) {};
    return &p;
}

const clap_plugin_factory kFactory = {
    [](const clap_plugin_factory *) -> uint32_t { return sizeof kDescriptors / sizeof kDescriptors[0]; },
    [](const clap_plugin_factory *, uint32_t index) -> const clap_plugin_descriptor * {
        return index < sizeof kDescriptors / sizeof kDescriptors[0] ? &kDescriptors[index] : nullptr;
    },
    [](const clap_plugin_factory *, const clap_host *host, const char *id) { return create(host, id); },
};

bool entryInit(const char *path)
{
    const std::string p = path ? path : "";
    const size_t slash = p.find_last_of("\\/");
    pluginFolder = slash == std::string::npos ? std::string() : p.substr(0, slash);
    return true;
}

}

extern "C" CLAP_EXPORT const clap_plugin_entry clap_entry = {
    CLAP_VERSION_INIT,
    entryInit,
    [] {},
    [](const char *id) -> const void * { return !std::strcmp(id, CLAP_PLUGIN_FACTORY_ID) ? &kFactory : nullptr; },
};
