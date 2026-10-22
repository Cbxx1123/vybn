// vibin-plugin-host.exe: runs one CLAP plugin for the app, sandboxed. The app starts it (in a job
// object and, where Windows allows, an AppContainer with no capabilities) and hands it, as
// inherited handles on the command line:
//
//   vibin-plugin-host <control-in> <control-out> <shared-memory> <process-event> <done-event>
//
// Control messages (plugin/Protocol.h) arrive on control-in and are answered on control-out, on
// this program's main thread, which is the plugin's main thread. The plugin's audio thread waits
// on the process event, runs one block from the shared memory and signals the done event. The
// host implements only these CLAP host extensions: log, thread-check, params, audio-ports,
// note-ports, state, latency and gui. A plugin's interface either opens as its own floating
// window, or (embedded) is parented to a frame window of this process that sits exactly over
// where the app draws it, invisible (layered, 1/255 opaque), just above the app's window in the
// stacking order (never topmost, so other programs' windows stay above it); the app moves it
// there itself, since from the sandbox the app's windows can't be seen. The real mouse and
// keyboard reach the plugin there directly, so it behaves as it would anywhere (its menus, lists
// and dialogs, top-level windows of its own, open at the right place at once). It is captured (60
// times a second while it is used, 25 otherwise) into shared memory for the app to draw beneath
// it. The app's mouse and keys can be replayed onto it too (while the frame is stood aside). Nothing of the plugin's is ever parented into
// the app's window (which would tie the app's input to a sandboxed, possibly hung, thread). A plugin asking for anything else gets nothing.
//
// The host exits when the app says Quit or its control pipe breaks (the app went away); the job
// object kills it anyway when the app closes it.

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00 // Windows 10 APIs; MinGW's headers otherwise stop at XP
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <xmmintrin.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <clap/clap.h>

#include "plugin/Protocol.h"

namespace {

using namespace plugin;

// --- The process's handles and state

HANDLE controlIn = nullptr;
HANDLE controlOut = nullptr;
HANDLE processEvent = nullptr;
HANDLE doneEvent = nullptr;
Shared *shared = nullptr;

HANDLE messageEvent = nullptr;  // the reader thread has a message
HANDLE callbackEvent = nullptr; // the plugin asked for on_main_thread
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

// News the plugin's threads leave for the main thread to send
std::mutex newsMutex;
std::vector<std::pair<int, std::string>> logs;
std::atomic<bool> callbackRequested{false};
std::atomic<bool> flushRequested{false};
std::atomic<bool> restartRequested{false};
std::atomic<bool> latencyChanged{false};
std::atomic<bool> stateDirty{false};
std::atomic<uint32_t> paramsRescan{0};
std::atomic<bool> guiClosed{false};

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

// --- The plugin

struct Port {
    clap_audio_buffer buffer{};
    std::vector<float *> channels;
    std::vector<std::vector<float>> scratch; // for ports that aren't the main one, and extra channels
};

constexpr int kNoPort = -1;

struct Loaded {
    HMODULE module = nullptr;
    const clap_plugin_entry *entry = nullptr;
    bool entryReady = false;
    const clap_plugin *plugin = nullptr;
    const clap_plugin_params *params = nullptr;
    const clap_plugin_audio_ports *audioPorts = nullptr;
    int sidechainInput = -1; // the first input port that isn't the main one, with 1 or 2 channels: the key's
    const clap_plugin_note_ports *notePorts = nullptr;
    const clap_plugin_state *state = nullptr;
    const clap_plugin_latency *latency = nullptr;
    const clap_plugin_gui *gui = nullptr;
    bool guiCreated = false;
    // Embedded: the window the interface is parented to, the capture and where it goes
    HWND frameWindow = nullptr;
    GuiFrame *frame = nullptr;
    HDC captureDC = nullptr;
    HBITMAP captureBitmap = nullptr;
    void *captureBits = nullptr;
    uint32_t captureWidth = 0, captureHeight = 0;
    ULONGLONG lastCapture = 0;
    ULONGLONG busyUntil = 0; // used recently: captured more often
    bool placed = false;     // the app has said where it is shown
    uint32_t mouseButtons = 0; // MK_ flags held
    HWND lastTarget = nullptr;
    uint32_t noteDialect = 0; // what notes are sent as: CLAP_NOTE_DIALECT_CLAP or _MIDI, or 0 for none
    bool midiControllers = false; // the note port takes MIDI, so controllers and the bend go as MIDI

    double sampleRate = 48000.0;
    std::atomic<bool> active{false};
    std::atomic<bool> paused{false}; // the audio thread answers with silence (while the main thread works)
    std::atomic<bool> stopRequest{false};
    bool processing = false; // audio thread

    std::vector<Port> inputs;
    std::vector<Port> outputs;
    std::vector<clap_audio_buffer> inBuffers; // one per port, pointing at its channels
    std::vector<clap_audio_buffer> outBuffers;
    int mainInput = -1;
    int mainOutput = -1;
};
Loaded loaded;

// --- Host extensions

void hostLog(const clap_host *, clap_log_severity severity, const char *message)
{
    std::lock_guard<std::mutex> lock(newsMutex);
    if (logs.size() < 200)
        logs.emplace_back(severity, std::string(message ? message : "").substr(0, 2000));
}

bool isMainThread(const clap_host *) { return GetCurrentThreadId() == mainThreadId; }
bool isAudioThread(const clap_host *) { return GetCurrentThreadId() == audioThreadId.load(); }

const clap_host_log kLog = {hostLog};
const clap_host_thread_check kThreadCheck = {isMainThread, isAudioThread};
const clap_host_params kParams = {
    [](const clap_host *, clap_param_rescan_flags flags) { paramsRescan.fetch_or(flags | 0x80000000u); },
    [](const clap_host *, clap_id, clap_param_clear_flags) {},
    [](const clap_host *) {
        flushRequested = true;
        SetEvent(callbackEvent);
    },
};
const clap_host_audio_ports kAudioPorts = {
    [](const clap_host *, uint32_t) { return false; },
    [](const clap_host *, uint32_t) {
        restartRequested = true;
        SetEvent(callbackEvent);
    },
};
const clap_host_note_ports kNotePorts = {
    [](const clap_host *) -> uint32_t { return CLAP_NOTE_DIALECT_CLAP | CLAP_NOTE_DIALECT_MIDI; },
    [](const clap_host *, uint32_t) {},
};
const clap_host_state kState = {[](const clap_host *) { stateDirty = true; }};
const clap_host_latency kLatency = {[](const clap_host *) { latencyChanged = true; }};
bool resizeFrame(uint32_t width, uint32_t height);

const clap_host_gui kGui = {
    [](const clap_host *) {},
    // A floating window sizes itself; an embedded one asks for its frame to grow or shrink
    [](const clap_host *, uint32_t w, uint32_t h) { return resizeFrame(w, h); },
    [](const clap_host *) { return false; },
    [](const clap_host *) { return false; },
    [](const clap_host *, bool) {
        guiClosed = true;
        SetEvent(callbackEvent);
    },
};

const void *hostExtension(const clap_host *, const char *id)
{
    if (!id)
        return nullptr;
    if (!std::strcmp(id, CLAP_EXT_LOG))
        return &kLog;
    if (!std::strcmp(id, CLAP_EXT_THREAD_CHECK))
        return &kThreadCheck;
    if (!std::strcmp(id, CLAP_EXT_PARAMS))
        return &kParams;
    if (!std::strcmp(id, CLAP_EXT_AUDIO_PORTS))
        return &kAudioPorts;
    if (!std::strcmp(id, CLAP_EXT_NOTE_PORTS))
        return &kNotePorts;
    if (!std::strcmp(id, CLAP_EXT_STATE))
        return &kState;
    if (!std::strcmp(id, CLAP_EXT_LATENCY))
        return &kLatency;
    if (!std::strcmp(id, CLAP_EXT_GUI))
        return &kGui;
    return nullptr; // everything else is deliberately not offered
}

const clap_host kHost = {
    CLAP_VERSION_INIT,
    nullptr,
    "Vibin",
    "Vibin",
    "",
    "1.0",
    hostExtension,
    [](const clap_host *) {
        restartRequested = true;
        SetEvent(callbackEvent);
    },
    [](const clap_host *) {},
    [](const clap_host *) {
        callbackRequested = true;
        SetEvent(callbackEvent);
    },
};

// --- Event lists

union EventStorage {
    clap_event_header header;
    clap_event_note note;
    clap_event_param_value param;
    clap_event_midi midi;
    clap_event_param_gesture gesture;
};

struct EventList {
    std::vector<EventStorage> items;
    uint32_t count = 0;
    clap_input_events in{};

    EventList()
    {
        items.resize(kMaxBlockEvents + 1100);
        in.ctx = this;
        in.size = [](const clap_input_events *list) { return static_cast<const EventList *>(list->ctx)->count; };
        in.get = [](const clap_input_events *list, uint32_t index) -> const clap_event_header * {
            const EventList *self = static_cast<const EventList *>(list->ctx);
            return index < self->count ? &self->items[index].header : nullptr;
        };
    }

    EventStorage *add()
    {
        if (count >= items.size())
            return nullptr;
        EventStorage *e = &items[count++];
        std::memset(e, 0, sizeof *e);
        return e;
    }

    void addParam(uint32_t time, clap_id id, double value)
    {
        if (EventStorage *e = add()) {
            e->param.header = {sizeof(clap_event_param_value), time, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_PARAM_VALUE, 0};
            e->param.param_id = id;
            e->param.note_id = -1;
            e->param.port_index = -1;
            e->param.channel = -1;
            e->param.key = -1;
            e->param.value = value;
        }
    }

    void addNote(uint32_t time, uint16_t type, int key, int channel, double velocity)
    {
        if (EventStorage *e = add()) {
            e->note.header = {sizeof(clap_event_note), time, CLAP_CORE_EVENT_SPACE_ID, type, 0};
            e->note.note_id = -1;
            e->note.port_index = 0;
            e->note.channel = static_cast<int16_t>(channel);
            e->note.key = static_cast<int16_t>(key);
            e->note.velocity = velocity;
        }
    }

    void addMidi(uint32_t time, uint8_t a, uint8_t b, uint8_t c)
    {
        if (EventStorage *e = add()) {
            e->midi.header = {sizeof(clap_event_midi), time, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_MIDI, 0};
            e->midi.port_index = 0;
            e->midi.data[0] = a;
            e->midi.data[1] = b;
            e->midi.data[2] = c;
        }
    }
};

// What the plugin sends back, to the app through the ring
const clap_output_events kOutEvents = {
    nullptr,
    [](const clap_output_events *, const clap_event_header *e) -> bool {
        if (!e || e->space_id != CLAP_CORE_EVENT_SPACE_ID)
            return true;
        ParamChange change;
        if (e->type == CLAP_EVENT_PARAM_VALUE) {
            const auto *p = reinterpret_cast<const clap_event_param_value *>(e);
            change = {ChangeKind::Value, p->param_id, p->value};
        } else if (e->type == CLAP_EVENT_PARAM_GESTURE_BEGIN || e->type == CLAP_EVENT_PARAM_GESTURE_END) {
            const auto *g = reinterpret_cast<const clap_event_param_gesture *>(e);
            change = {e->type == CLAP_EVENT_PARAM_GESTURE_BEGIN ? ChangeKind::GestureBegin : ChangeKind::GestureEnd, g->param_id, 0.0};
        } else {
            return true;
        }
        return shared->fromPlugin.push(change);
    },
};

// --- Audio thread

EventList *blockEvents = nullptr; // made before the audio thread starts

void silence(uint32_t frames)
{
    for (uint32_t c = 0; c < kMaxChannels; ++c)
        std::memset(shared->output[c], 0, frames * sizeof(float));
    shared->outputChannels = kMaxChannels;
}

void runBlock()
{
    const uint32_t frames = shared->frames <= kMaxFrames ? shared->frames : kMaxFrames;
    Loaded &l = loaded;
    if (!l.active || l.paused || l.mainOutput < 0) {
        silence(frames);
        shared->status = CLAP_PROCESS_SLEEP;
        return;
    }
    if (!l.processing) {
        l.processing = l.plugin->start_processing(l.plugin);
        if (!l.processing) {
            silence(frames);
            shared->status = CLAP_PROCESS_ERROR;
            return;
        }
    }

    // Events: the app's parameter changes first, then the block's own, in time order
    EventList &ev = *blockEvents;
    ev.count = 0;
    ParamChange change;
    while (shared->toPlugin.pop(change))
        if (change.kind == ChangeKind::Value)
            ev.addParam(0, change.id, change.value);
    const uint32_t count = shared->eventCount <= kMaxBlockEvents ? shared->eventCount : kMaxBlockEvents;
    for (uint32_t i = 0; i < count; ++i) {
        const Event &e = shared->events[i];
        const uint32_t time = e.frame < frames ? e.frame : (frames > 0 ? frames - 1 : 0);
        const int channel = e.channel >= 0 && e.channel < 16 ? e.channel : 0;
        switch (e.kind) {
        case EventKind::ParamValue:
            ev.addParam(time, e.id, e.value);
            break;
        case EventKind::NoteOn:
        case EventKind::NoteOff: {
            const int key = static_cast<int>(e.id & 127);
            const bool on = e.kind == EventKind::NoteOn;
            if (l.noteDialect == CLAP_NOTE_DIALECT_CLAP)
                ev.addNote(time, on ? CLAP_EVENT_NOTE_ON : CLAP_EVENT_NOTE_OFF, key, channel, e.value);
            else if (l.noteDialect == CLAP_NOTE_DIALECT_MIDI) {
                int velocity = static_cast<int>(e.value * 127.0 + 0.5);
                velocity = velocity < 1 ? 1 : (velocity > 127 ? 127 : velocity);
                ev.addMidi(time, static_cast<uint8_t>((on ? 0x90 : 0x80) | channel), static_cast<uint8_t>(key),
                           static_cast<uint8_t>(on ? velocity : 64));
            }
            break;
        }
        case EventKind::Controller: {
            if (!l.midiControllers)
                break;
            const double v = e.value < 0.0 ? 0.0 : (e.value > 1.0 ? 1.0 : e.value);
            if (e.id == 128) {
                const int bend = static_cast<int>(v * 16383.0 + 0.5);
                ev.addMidi(time, static_cast<uint8_t>(0xE0 | channel), static_cast<uint8_t>(bend & 127),
                           static_cast<uint8_t>((bend >> 7) & 127));
            } else if (e.id < 128) {
                ev.addMidi(time, static_cast<uint8_t>(0xB0 | channel), static_cast<uint8_t>(e.id),
                           static_cast<uint8_t>(v * 127.0 + 0.5));
            }
            break;
        }
        case EventKind::AllNotesOff:
            if (l.noteDialect == CLAP_NOTE_DIALECT_CLAP)
                ev.addNote(time, CLAP_EVENT_NOTE_OFF, -1, -1, 0.0);
            else if (l.noteDialect == CLAP_NOTE_DIALECT_MIDI)
                ev.addMidi(time, static_cast<uint8_t>(0xB0 | channel), 123, 0);
            break;
        }
    }
    // Times must not go backwards; the app sends them in order, but keep the promise
    for (uint32_t i = 1; i < ev.count; ++i)
        if (ev.items[i].header.time < ev.items[i - 1].header.time)
            ev.items[i].header.time = ev.items[i - 1].header.time;

    // Transport
    clap_event_transport transport{};
    transport.header = {sizeof transport, 0, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_TRANSPORT, 0};
    const TransportInfo &t = shared->transport;
    transport.flags = CLAP_TRANSPORT_HAS_TEMPO | CLAP_TRANSPORT_HAS_BEATS_TIMELINE | CLAP_TRANSPORT_HAS_SECONDS_TIMELINE |
                      CLAP_TRANSPORT_HAS_TIME_SIGNATURE | (t.playing ? CLAP_TRANSPORT_IS_PLAYING : 0);
    transport.tempo = t.tempo > 1.0 && t.tempo < 1000.0 ? t.tempo : 120.0;
    transport.song_pos_beats = static_cast<clap_beattime>(t.beats * CLAP_BEATTIME_FACTOR);
    transport.song_pos_seconds = static_cast<clap_sectime>(t.seconds * CLAP_SECTIME_FACTOR);
    const uint32_t beatsPerBar = t.beatsPerBar > 0 && t.beatsPerBar < 64 ? t.beatsPerBar : 4;
    transport.tsig_num = static_cast<uint16_t>(beatsPerBar);
    transport.tsig_denom = 4;
    transport.bar_number = static_cast<int32_t>(t.beats / beatsPerBar);
    transport.bar_start = static_cast<clap_beattime>(transport.bar_number * static_cast<double>(beatsPerBar) * CLAP_BEATTIME_FACTOR);

    // Audio: the main ports read and write the shared block
    if (l.mainInput >= 0) {
        Port &in = l.inputs[static_cast<size_t>(l.mainInput)];
        const uint32_t given = shared->inputChannels <= kMaxChannels ? shared->inputChannels : kMaxChannels;
        for (uint32_t c = 0; c < in.channels.size() && c < kMaxChannels; ++c)
            in.channels[c] = given > 0 ? shared->input[c < given ? c : given - 1] : in.scratch[c].data();
    }
    // The sidechain port: the app's key when there is one, else its own silent buffers (a port must
    // always have buffers)
    if (l.sidechainInput >= 0) {
        Port &side = l.inputs[static_cast<size_t>(l.sidechainInput)];
        const uint32_t given = shared->keyChannels <= kMaxChannels ? shared->keyChannels : kMaxChannels;
        for (uint32_t c = 0; c < side.channels.size() && c < kMaxChannels; ++c)
            side.channels[c] = given > 0 ? shared->key[c < given ? c : given - 1] : side.scratch[c].data();
    }
    Port &out = l.outputs[static_cast<size_t>(l.mainOutput)];
    for (uint32_t c = 0; c < out.channels.size() && c < kMaxChannels; ++c)
        out.channels[c] = shared->output[c];

    clap_process process{};
    static int64_t steady = 0;
    process.steady_time = steady;
    steady += frames;
    process.frames_count = frames;
    process.transport = &transport;
    process.audio_inputs = l.inBuffers.data();
    process.audio_inputs_count = static_cast<uint32_t>(l.inBuffers.size());
    process.audio_outputs = l.outBuffers.data();
    process.audio_outputs_count = static_cast<uint32_t>(l.outBuffers.size());
    process.in_events = &ev.in;
    process.out_events = &kOutEvents;

    const clap_process_status status = l.plugin->process(l.plugin, &process);
    shared->status = status;
    if (status == CLAP_PROCESS_ERROR) {
        silence(frames);
        return;
    }
    // A mono output plays on both sides
    if (out.channels.size() == 1)
        std::memcpy(shared->output[1], shared->output[0], frames * sizeof(float));
    shared->outputChannels = kMaxChannels;
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
        if (loaded.stopRequest) {
            if (loaded.processing) {
                loaded.plugin->stop_processing(loaded.plugin);
                loaded.processing = false;
            }
            loaded.stopRequest = false;
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
    loaded.paused = true;
    loaded.stopRequest = true;
    SetEvent(wakeAudio);
    WaitForSingleObject(audioAck, 5000);
}

// --- Main thread: the requests

std::wstring widen(const std::string &s)
{
    if (s.empty())
        return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), &w[0], n);
    return w;
}

bool openModule(const std::string &path, std::string &error)
{
    if (loaded.module)
        return true;
    // Only the plugin's own folder and the system's are searched for its DLLs
    loaded.module = LoadLibraryExW(widen(path).c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (!loaded.module) {
        error = "Couldn't load the plugin's file (error " + std::to_string(GetLastError()) + ")";
        return false;
    }
    loaded.entry = reinterpret_cast<const clap_plugin_entry *>(GetProcAddress(loaded.module, "clap_entry"));
    if (!loaded.entry) {
        error = "Not a CLAP plugin: it has no clap_entry";
        return false;
    }
    if (!clap_version_is_compatible(loaded.entry->clap_version)) {
        error = "The plugin was built for an incompatible CLAP version";
        return false;
    }
    if (!loaded.entry->init(path.c_str())) {
        error = "The plugin refused to start (its entry's init failed)";
        return false;
    }
    loaded.entryReady = true;
    return true;
}

const clap_plugin_factory *factory()
{
    return static_cast<const clap_plugin_factory *>(loaded.entry->get_factory(CLAP_PLUGIN_FACTORY_ID));
}

std::string joinFeatures(const char *const *features)
{
    std::string out;
    for (int i = 0; features && features[i] && i < 64; ++i) {
        if (!out.empty())
            out += ' ';
        out += features[i];
    }
    return out;
}

// The channels of a plugin's sidechain input: the first input port that isn't the main one, if it has
// one or two (the rule buildPorts follows); 0 for none
uint32_t sidechainChannels(const clap_plugin *p)
{
    const auto *ports = static_cast<const clap_plugin_audio_ports *>(p->get_extension(p, CLAP_EXT_AUDIO_PORTS));
    if (!ports)
        return 0;
    const uint32_t n = ports->count(p, true);
    int main = -1;
    std::vector<uint32_t> channels;
    for (uint32_t i = 0; i < n && i < 32; ++i) {
        clap_audio_port_info info{};
        if (!ports->get(p, i, true, &info))
            continue;
        if (main < 0 && (info.flags & CLAP_AUDIO_PORT_IS_MAIN))
            main = static_cast<int>(channels.size());
        channels.push_back(info.channel_count);
    }
    if (main < 0 && !channels.empty())
        main = 0;
    for (size_t i = 0; i < channels.size(); ++i) {
        if (static_cast<int>(i) != main && (channels[i] == 1 || channels[i] == 2))
            return channels[i];
    }
    return 0;
}

void handleScan(uint32_t sequence, Reader &r)
{
    const std::string path = r.str();
    std::string error;
    if (!openModule(path, error))
        return fail(sequence, error);
    const clap_plugin_factory *f = factory();
    if (!f)
        return fail(sequence, "The plugin has no plugin factory");
    const uint32_t count = f->get_plugin_count(f);
    Writer w;
    std::vector<Descriptor> found;
    for (uint32_t i = 0; i < count && i < 1000; ++i) {
        const clap_plugin_descriptor *d = f->get_plugin_descriptor(f, i);
        if (!d || !d->id || !d->name)
            continue;
        Descriptor out;
        out.id = d->id;
        out.name = d->name;
        out.vendor = d->vendor ? d->vendor : "";
        out.version = d->version ? d->version : "";
        out.description = d->description ? d->description : "";
        out.url = d->url ? d->url : "";
        out.features = joinFeatures(d->features);
        found.push_back(out);
    }
    w.u32(static_cast<uint32_t>(found.size()));
    for (const Descriptor &d : found)
        writeDescriptor(w, d);
    send(Message::Reply, sequence, w.data);
}

// The sidechain port of one plugin, in a host of its own (the scan's descriptors are already safe)
void handleProbe(uint32_t sequence, Reader &r)
{
    const std::string path = r.str();
    const std::string id = r.str();
    std::string error;
    if (!openModule(path, error))
        return fail(sequence, error);
    const clap_plugin_factory *f = factory();
    if (!f)
        return fail(sequence, "The plugin has no plugin factory");
    const clap_plugin *p = f->create_plugin(f, &kHost, id.c_str());
    if (!p)
        return fail(sequence, "The plugin file has no plugin \"" + id + "\"");
    uint32_t channels = 0;
    if (p->init(p))
        channels = sidechainChannels(p);
    p->destroy(p);
    Writer w;
    w.u32(channels);
    send(Message::Reply, sequence, w.data);
}

void handleLoad(uint32_t sequence, Reader &r)
{
    const std::string path = r.str();
    const std::string id = r.str();
    if (loaded.plugin)
        return fail(sequence, "A plugin is already loaded");
    std::string error;
    if (!openModule(path, error))
        return fail(sequence, error);
    const clap_plugin_factory *f = factory();
    if (!f)
        return fail(sequence, "The plugin has no plugin factory");
    const clap_plugin *p = f->create_plugin(f, &kHost, id.c_str());
    if (!p)
        return fail(sequence, "The plugin file has no plugin \"" + id + "\"");
    if (!p->init(p)) {
        p->destroy(p);
        return fail(sequence, "The plugin failed to start (init)");
    }
    loaded.plugin = p;
    loaded.params = static_cast<const clap_plugin_params *>(p->get_extension(p, CLAP_EXT_PARAMS));
    loaded.audioPorts = static_cast<const clap_plugin_audio_ports *>(p->get_extension(p, CLAP_EXT_AUDIO_PORTS));
    loaded.notePorts = static_cast<const clap_plugin_note_ports *>(p->get_extension(p, CLAP_EXT_NOTE_PORTS));
    loaded.state = static_cast<const clap_plugin_state *>(p->get_extension(p, CLAP_EXT_STATE));
    loaded.latency = static_cast<const clap_plugin_latency *>(p->get_extension(p, CLAP_EXT_LATENCY));
    loaded.gui = static_cast<const clap_plugin_gui *>(p->get_extension(p, CLAP_EXT_GUI));

    LoadedInfo info;
    info.name = p->desc && p->desc->name ? p->desc->name : id;
    if (loaded.audioPorts) {
        for (int input = 1; input >= 0; --input) {
            const uint32_t n = loaded.audioPorts->count(p, input != 0);
            for (uint32_t i = 0; i < n; ++i) {
                clap_audio_port_info port{};
                if (loaded.audioPorts->get(p, i, input != 0, &port) && (port.flags & CLAP_AUDIO_PORT_IS_MAIN || i == 0)) {
                    (input ? info.inputChannels : info.outputChannels) = port.channel_count;
                    if (port.flags & CLAP_AUDIO_PORT_IS_MAIN)
                        break;
                }
            }
        }
    }
    if (loaded.notePorts) {
        info.notePorts = loaded.notePorts->count(p, true);
        clap_note_port_info port{};
        if (info.notePorts > 0 && loaded.notePorts->get(p, 0, true, &port)) {
            if (port.supported_dialects & CLAP_NOTE_DIALECT_CLAP)
                loaded.noteDialect = CLAP_NOTE_DIALECT_CLAP;
            else if (port.supported_dialects & CLAP_NOTE_DIALECT_MIDI)
                loaded.noteDialect = CLAP_NOTE_DIALECT_MIDI;
            loaded.midiControllers = (port.supported_dialects & CLAP_NOTE_DIALECT_MIDI) != 0;
        }
    }
    info.params = loaded.params != nullptr;
    info.state = loaded.state != nullptr;

    Writer w;
    w.str(info.name);
    w.u32(info.inputChannels);
    w.u32(info.outputChannels);
    w.u32(info.notePorts);
    w.u32(info.params);
    w.u32(info.state);
    uint32_t guiFlags = 0;
    if (loaded.gui && loaded.gui->is_api_supported(p, CLAP_WINDOW_API_WIN32, true))
        guiFlags |= 1;
    if (loaded.gui && loaded.gui->is_api_supported(p, CLAP_WINDOW_API_WIN32, false))
        guiFlags |= 2;
    w.u32(guiFlags);
    send(Message::Reply, sequence, w.data);
}

void releaseCapture()
{
    if (loaded.captureBitmap)
        DeleteObject(loaded.captureBitmap);
    if (loaded.captureDC)
        DeleteDC(loaded.captureDC);
    loaded.captureBitmap = nullptr;
    loaded.captureDC = nullptr;
    loaded.captureBits = nullptr;
    loaded.captureWidth = loaded.captureHeight = 0;
}

void closeGui()
{
    if (loaded.guiCreated) {
        loaded.gui->hide(loaded.plugin);
        loaded.gui->destroy(loaded.plugin);
        loaded.guiCreated = false;
    }
    if (loaded.frameWindow) {
        DestroyWindow(loaded.frameWindow);
        loaded.frameWindow = nullptr;
    }
    releaseCapture();
    if (loaded.frame) {
        UnmapViewOfFile(loaded.frame);
        loaded.frame = nullptr;
    }
    loaded.mouseButtons = 0;
    loaded.lastTarget = nullptr;
}

bool resizeFrame(uint32_t width, uint32_t height)
{
    if (!loaded.frameWindow)
        return false;
    width = width < 16 ? 16 : (width > kGuiMaxWidth ? kGuiMaxWidth : width);
    height = height < 16 ? 16 : (height > kGuiMaxHeight ? kGuiMaxHeight : height);
    SetWindowPos(loaded.frameWindow, nullptr, 0, 0, static_cast<int>(width), static_cast<int>(height), SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    return true;
}

// Where the frame waits until the app says where it is shown (and while it isn't): off every
// screen, still shown so the system keeps drawing it
const int kAwayX = -32000 + 8000;
const int kAwayY = -32000 + 8000;

// Where the frame's top-left corner is on the desktop now
POINT frameOrigin()
{
    POINT p{0, 0};
    if (loaded.frameWindow)
        ClientToScreen(loaded.frameWindow, &p);
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
    if (!loaded.plugin || !loaded.gui)
        return fail(sequence, "This plugin has no interface");
    if (!loaded.gui->is_api_supported(loaded.plugin, CLAP_WINDOW_API_WIN32, false))
        return fail(sequence, "This plugin's interface only opens as a window of its own");
    closeGui();
    loaded.frame = static_cast<GuiFrame *>(MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(GuiFrame)));
    CloseHandle(mapping);
    if (!loaded.frame)
        return fail(sequence, "Couldn't reach the app's picture of the interface");
    static bool registered = false;
    if (!registered) {
        WNDCLASSW wc{};
        wc.lpfnWndProc = frameProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hbrBackground = reinterpret_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
        wc.lpszClassName = L"VibinPluginFrame";
        registered = RegisterClassW(&wc) != 0;
    }
    // Invisible but taking the mouse: the app shows the picture beneath it
    loaded.frameWindow = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_LAYERED, L"VibinPluginFrame", L"",
                                         WS_POPUP | WS_CLIPCHILDREN, kAwayX, kAwayY, 640, 400, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!loaded.frameWindow) {
        closeGui();
        return fail(sequence, "Couldn't make a window for the interface");
    }
    SetLayeredWindowAttributes(loaded.frameWindow, 0, 1, LWA_ALPHA);
    loaded.placed = false;
    if (!loaded.gui->create(loaded.plugin, CLAP_WINDOW_API_WIN32, false)) {
        closeGui();
        return fail(sequence, "The plugin couldn't make its interface");
    }
    loaded.guiCreated = true;
    loaded.gui->set_scale(loaded.plugin, 1.0);
    uint32_t width = 640, height = 400;
    if (!loaded.gui->get_size(loaded.plugin, &width, &height) || width == 0 || height == 0)
        width = 640, height = 400;
    resizeFrame(width, height);
    clap_window window{};
    window.api = CLAP_WINDOW_API_WIN32;
    window.win32 = loaded.frameWindow;
    if (!loaded.gui->set_parent(loaded.plugin, &window)) {
        closeGui();
        return fail(sequence, "The plugin couldn't place its interface");
    }
    ShowWindow(loaded.frameWindow, SW_SHOWNOACTIVATE);
    loaded.gui->show(loaded.plugin);
    loaded.lastCapture = 0;
    RECT rc;
    GetClientRect(loaded.frameWindow, &rc);
    Writer w;
    w.u32(static_cast<uint32_t>(rc.right - rc.left));
    w.u32(static_cast<uint32_t>(rc.bottom - rc.top));
    // The app places and stacks the frame itself: from the sandbox the app's windows can't even
    // be seen, let alone stacked against, while the app may move this process's windows
    w.u32(static_cast<uint32_t>(reinterpret_cast<uintptr_t>(loaded.frameWindow)));
    send(Message::Reply, sequence, w.data);
}

// Copies the interface into the shared frame: 60 times a second while it is used, 25 otherwise
void captureGui()
{
    if (!loaded.frameWindow || !loaded.frame)
        return;
    const ULONGLONG now = GetTickCount64();
    if (now - loaded.lastCapture < (now < loaded.busyUntil ? 15u : 40u))
        return;
    loaded.lastCapture = now;
    RECT rc;
    GetClientRect(loaded.frameWindow, &rc);
    const uint32_t width = static_cast<uint32_t>(rc.right - rc.left) < kGuiMaxWidth ? static_cast<uint32_t>(rc.right - rc.left) : kGuiMaxWidth;
    const uint32_t height = static_cast<uint32_t>(rc.bottom - rc.top) < kGuiMaxHeight ? static_cast<uint32_t>(rc.bottom - rc.top) : kGuiMaxHeight;
    if (width == 0 || height == 0)
        return;
    if (width != loaded.captureWidth || height != loaded.captureHeight) {
        releaseCapture();
        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof info.bmiHeader;
        info.bmiHeader.biWidth = static_cast<LONG>(width);
        info.bmiHeader.biHeight = -static_cast<LONG>(height); // top-down
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        HDC screen = GetDC(nullptr);
        loaded.captureDC = CreateCompatibleDC(screen);
        loaded.captureBitmap = CreateDIBSection(screen, &info, DIB_RGB_COLORS, &loaded.captureBits, nullptr, 0);
        ReleaseDC(nullptr, screen);
        if (!loaded.captureBitmap) {
            releaseCapture();
            return;
        }
        SelectObject(loaded.captureDC, loaded.captureBitmap);
        loaded.captureWidth = width;
        loaded.captureHeight = height;
    }
    // The system's copy of the window, children included, read without making the plugin draw
    // again. If that comes out one flat colour (the frame not drawn yet, or off the screen), the
    // window is asked to paint into the capture instead, which costs a full redraw.
    HDC source = GetDCEx(loaded.frameWindow, nullptr, DCX_WINDOW | DCX_CACHE);
    BitBlt(loaded.captureDC, 0, 0, static_cast<int>(width), static_cast<int>(height), source, 0, 0, SRCCOPY);
    ReleaseDC(loaded.frameWindow, source);
    GdiFlush();
    auto isFlat = [&] {
        const uint32_t *px = static_cast<const uint32_t *>(loaded.captureBits);
        for (size_t i = 1, n = static_cast<size_t>(width) * height; i < n; i += 7)
            if ((px[i] & 0xFFFFFF) != (px[0] & 0xFFFFFF))
                return false;
        return true;
    };
    if (isFlat()) {
        PrintWindow(loaded.frameWindow, loaded.captureDC, 0);
        GdiFlush();
    }
    GuiFrame &f = *loaded.frame;
    const uint32_t seq = f.sequence.load(std::memory_order_relaxed);
    f.sequence.store(seq | 1, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
    f.width = width;
    f.height = height;
    std::memcpy(f.pixels, loaded.captureBits, static_cast<size_t>(width) * height * 4);
    std::atomic_thread_fence(std::memory_order_release);
    f.sequence.store((seq | 1) + 1, std::memory_order_release);
}

// The deepest visible child of the frame under a point (frame pixels), or the window holding the
// mouse while a drag is under way
HWND inputTarget(POINT at)
{
    if (HWND captured = GetCapture())
        return captured;
    HWND w = loaded.frameWindow;
    for (int depth = 0; depth < 32; ++depth) {
        POINT local = at;
        MapWindowPoints(loaded.frameWindow, w, &local, 1);
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
    if (!r.ok() || !loaded.frameWindow)
        return;
    loaded.busyUntil = GetTickCount64() + 1000;
    const POINT at{in.x, in.y};
    const POINT origin = frameOrigin();
    WPARAM held = loaded.mouseButtons;
    if (in.mods & 1)
        held |= MK_SHIFT;
    if (in.mods & 2)
        held |= MK_CONTROL;
    auto toTarget = [&](HWND target) {
        POINT local = at;
        MapWindowPoints(loaded.frameWindow, target, &local, 1);
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
        if (loaded.lastTarget && loaded.lastTarget != target && IsWindow(loaded.lastTarget))
            PostMessageW(loaded.lastTarget, WM_MOUSELEAVE, 0, 0);
        loaded.lastTarget = target;
        PostMessageW(target, WM_MOUSEMOVE, held, toTarget(target));
        break;
    }
    case GuiInputKind::Down:
    case GuiInputKind::DoubleClick: {
        HWND target = inputTarget(at);
        loaded.mouseButtons |= static_cast<uint32_t>(flags[b]);
        const bool dbl = in.kind == GuiInputKind::DoubleClick && (GetClassLongW(target, GCL_STYLE) & CS_DBLCLKS);
        PostMessageW(target, dbl ? doubles[b] : downs[b], held | flags[b], toTarget(target));
        break;
    }
    case GuiInputKind::Up: {
        HWND target = inputTarget(at);
        loaded.mouseButtons &= ~static_cast<uint32_t>(flags[b]);
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
        if (loaded.lastTarget && IsWindow(loaded.lastTarget))
            PostMessageW(loaded.lastTarget, WM_MOUSELEAVE, 0, 0);
        loaded.lastTarget = nullptr;
        break;
    case GuiInputKind::KeyDown:
    case GuiInputKind::KeyUp:
    case GuiInputKind::Char: {
        HWND target = GetFocus();
        if (!target || (target != loaded.frameWindow && !IsChild(loaded.frameWindow, target)))
            target = loaded.lastTarget && IsWindow(loaded.lastTarget) ? loaded.lastTarget : inputTarget(at);
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
    if (!r.ok() || !loaded.frameWindow)
        return;
    if (w <= 0 || h <= 0)
        SetWindowRgn(loaded.frameWindow, nullptr, TRUE);
    else
        SetWindowRgn(loaded.frameWindow, CreateRectRgn(x, y, x + w, y + h), TRUE); // the system keeps the region
}

void handleGuiResize(Reader &r)
{
    uint32_t width = r.u32(), height = r.u32();
    if (!r.ok() || !loaded.frameWindow || !loaded.guiCreated || !loaded.gui->can_resize(loaded.plugin))
        return;
    loaded.gui->adjust_size(loaded.plugin, &width, &height);
    if (loaded.gui->set_size(loaded.plugin, width, height))
        resizeFrame(width, height);
}

void handleGuiOpen(uint32_t sequence)
{
    if (!loaded.plugin || !loaded.gui)
        return fail(sequence, "This plugin has no window of its own");
    if (!loaded.guiCreated) {
        if (!loaded.gui->is_api_supported(loaded.plugin, CLAP_WINDOW_API_WIN32, true))
            return fail(sequence, "This plugin's interface only opens inside a host's window, which Vibin doesn't offer yet");
        if (!loaded.gui->create(loaded.plugin, CLAP_WINDOW_API_WIN32, true))
            return fail(sequence, "The plugin couldn't make its window");
        loaded.guiCreated = true;
        if (loaded.plugin->desc && loaded.plugin->desc->name)
            loaded.gui->suggest_title(loaded.plugin, loaded.plugin->desc->name);
    }
    if (!loaded.gui->show(loaded.plugin))
        return fail(sequence, "The plugin couldn't show its window");
    send(Message::Reply, sequence);
}

// Builds the port buffers for the plugin's current ports
void buildPorts()
{
    const clap_plugin *p = loaded.plugin;
    for (int input = 1; input >= 0; --input) {
        std::vector<Port> &ports = input ? loaded.inputs : loaded.outputs;
        int &main = input ? loaded.mainInput : loaded.mainOutput;
        ports.clear();
        main = -1;
        const uint32_t n = loaded.audioPorts ? loaded.audioPorts->count(p, input != 0) : 0;
        for (uint32_t i = 0; i < n && i < 32; ++i) {
            clap_audio_port_info info{};
            if (!loaded.audioPorts->get(p, i, input != 0, &info))
                continue;
            Port port;
            const uint32_t channels = info.channel_count < 64 ? info.channel_count : 64;
            port.scratch.assign(channels, std::vector<float>(kMaxFrames, 0.0f));
            for (uint32_t c = 0; c < channels; ++c)
                port.channels.push_back(port.scratch[c].data());
            port.buffer.channel_count = channels;
            if (main < 0 && (info.flags & CLAP_AUDIO_PORT_IS_MAIN))
                main = static_cast<int>(ports.size());
            ports.push_back(std::move(port));
        }
        if (main < 0 && !ports.empty())
            main = 0;
        if (input) {
            loaded.sidechainInput = kNoPort;
            for (size_t i = 0; i < ports.size(); ++i) {
                if (static_cast<int>(i) != main && (ports[i].scratch.size() == 1 || ports[i].scratch.size() == 2)) {
                    loaded.sidechainInput = static_cast<int>(i);
                    break;
                }
            }
        }
        std::vector<clap_audio_buffer> &buffers = input ? loaded.inBuffers : loaded.outBuffers;
        buffers.clear();
        for (Port &port : ports) {
            port.buffer.data32 = port.channels.data();
            buffers.push_back(port.buffer);
        }
    }
}

bool activate(double rate, std::string &error)
{
    buildPorts();
    loaded.sampleRate = rate;
    if (!loaded.plugin->activate(loaded.plugin, rate, 1, kMaxFrames)) {
        error = "The plugin refused to activate";
        return false;
    }
    shared->latency.store(loaded.latency ? loaded.latency->get(loaded.plugin) : 0);
    loaded.paused = false;
    loaded.active = true;
    return true;
}

void deactivate()
{
    if (!loaded.active)
        return;
    stopAudio();
    loaded.active = false;
    loaded.plugin->deactivate(loaded.plugin);
    loaded.paused = false;
}

void handleActivate(uint32_t sequence, Reader &r)
{
    const double rate = r.f64();
    if (!loaded.plugin)
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
    Writer w;
    std::vector<ParamInfo> list;
    if (loaded.params) {
        const uint32_t n = loaded.params->count(loaded.plugin);
        for (uint32_t i = 0; i < n && i < 10000; ++i) {
            clap_param_info info{};
            if (!loaded.params->get_info(loaded.plugin, i, &info))
                continue;
            ParamInfo p;
            p.id = info.id;
            p.flags = info.flags;
            info.name[sizeof info.name - 1] = 0;
            info.module[sizeof info.module - 1] = 0;
            p.name = info.name;
            p.module = info.module;
            p.min = info.min_value;
            p.max = info.max_value;
            p.def = info.default_value;
            double value = info.default_value;
            if (loaded.params->get_value(loaded.plugin, info.id, &value))
                p.value = value;
            else
                p.value = info.default_value;
            list.push_back(p);
        }
    }
    w.u32(static_cast<uint32_t>(list.size()));
    for (const ParamInfo &p : list)
        writeParam(w, p);
    send(Message::Reply, sequence, w.data);
}

void handleParamText(uint32_t sequence, Reader &r)
{
    const uint32_t id = r.u32();
    const double value = r.f64();
    char text[256] = {};
    if (!loaded.params || !loaded.params->value_to_text(loaded.plugin, id, value, text, sizeof text))
        return fail(sequence, "No text");
    text[sizeof text - 1] = 0;
    Writer w;
    w.str(text);
    send(Message::Reply, sequence, w.data);
}

struct OutStream {
    std::string data;
    clap_ostream stream{this, [](const clap_ostream *s, const void *buffer, uint64_t size) -> int64_t {
                            OutStream *self = static_cast<OutStream *>(s->ctx);
                            if (self->data.size() + size > kMaxMessageBytes - 64)
                                return -1;
                            self->data.append(static_cast<const char *>(buffer), static_cast<size_t>(size));
                            return static_cast<int64_t>(size);
                        }};
};

struct InStream {
    const std::string *data = nullptr;
    size_t at = 0;
    clap_istream stream{this, [](const clap_istream *s, void *buffer, uint64_t size) -> int64_t {
                            InStream *self = static_cast<InStream *>(s->ctx);
                            const size_t left = self->data->size() - self->at;
                            const size_t n = size < left ? static_cast<size_t>(size) : left;
                            std::memcpy(buffer, self->data->data() + self->at, n);
                            self->at += n;
                            return static_cast<int64_t>(n);
                        }};
};

void handleSaveState(uint32_t sequence)
{
    if (!loaded.state)
        return send(Message::Reply, sequence, std::string());
    OutStream out;
    if (!loaded.state->save(loaded.plugin, &out.stream))
        return fail(sequence, "The plugin couldn't save its state");
    send(Message::Reply, sequence, out.data);
}

void handleLoadState(uint32_t sequence, const std::string &payload)
{
    if (!loaded.state)
        return fail(sequence, "The plugin has no state to load");
    InStream in;
    in.data = &payload;
    if (!loaded.state->load(loaded.plugin, &in.stream))
        return fail(sequence, "The plugin couldn't load the state");
    send(Message::Reply, sequence);
}

void quit()
{
    closeGui();
    if (loaded.plugin) {
        deactivate();
        loaded.plugin->destroy(loaded.plugin);
        loaded.plugin = nullptr;
    }
    if (loaded.entryReady)
        loaded.entry->deinit();
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
        if (loaded.plugin)
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

// Passes on what the plugin's threads asked for, and flushes parameters while it isn't processing
void housekeeping()
{
    if (!loaded.plugin)
        return;
    if (callbackRequested.exchange(false))
        loaded.plugin->on_main_thread(loaded.plugin);
    if (restartRequested.exchange(false) && loaded.active) {
        const double rate = loaded.sampleRate;
        deactivate();
        std::string error;
        if (!activate(rate, error))
            hostLog(&kHost, CLAP_LOG_ERROR, ("Restart failed: " + error).c_str());
        latencyChanged = true;
    }
    // While inactive the app's changes reach the plugin through flush
    const bool flush = flushRequested.exchange(false);
    if (!loaded.active && loaded.params) {
        EventList ev;
        ParamChange change;
        while (shared->toPlugin.pop(change))
            if (change.kind == ChangeKind::Value)
                ev.addParam(0, change.id, change.value);
        if (ev.count > 0 || flush)
            loaded.params->flush(loaded.plugin, &ev.in, &kOutEvents);
    }
    if (latencyChanged.exchange(false)) {
        const uint32_t frames = loaded.latency && loaded.active ? loaded.latency->get(loaded.plugin) : 0;
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

int main(int argc, char **argv)
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
    blockEvents = new EventList;

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
