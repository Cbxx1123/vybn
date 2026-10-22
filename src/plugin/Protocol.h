#pragma once

// What the app and its sandboxed plugin hosts (vybn-plugin-host.exe, one process per plugin
// instance) share. Both sides include this file and are built by the same compiler, so the
// layouts agree.
//
// Two channels:
// - The control channel, a pair of anonymous pipes carrying framed messages (Message, Writer,
//   Reader): loading, activating, parameter lists, state. Never used on the audio thread.
// - The audio channel, one shared memory block per instance (Shared): the block's audio in and
//   out, its timed events, the transport, and two rings of parameter changes. The app's audio
//   thread fills a request, bumps `request` and signals the host's process event; the host's
//   audio thread runs the plugin, sets `done` and signals back. Nothing in it locks or allocates.
//
// The app trusts nothing the host writes: a plugin is untrusted code, and the host is the plugin's
// process. Every index read from shared memory is masked, every count clamped and every sample
// checked for NaN before it reaches the mix.

#include <atomic>
#include <cstdint>
#include <cstring>
#include <string>

namespace plugin {

constexpr uint32_t kMagic = 0x48504256; // "VBPH"
constexpr uint32_t kProtocolVersion = 4; // 2: the sidechain key in the block; 3: the Probe request;
                                         // 4: the time signature, bar and tempo ramp in the transport
constexpr uint32_t kMaxFrames = 4096;    // a block; longer ones are split
constexpr uint32_t kMaxChannels = 2;     // of the main ports: stereo
constexpr uint32_t kMaxBlockEvents = 512;
constexpr uint32_t kMaxMessageBytes = 64u << 20; // a state can be large; anything bigger is refused

// A timed event for one block, written by the app's audio thread
enum class EventKind : uint32_t {
    NoteOn,      // key, channel, value = velocity 0-1
    NoteOff,     // key, channel, value = release velocity
    AllNotesOff, // every key (the transport stopped or jumped)
    ParamValue,  // id = parameter id, value = plain value
    Controller,  // id = MIDI controller 0-127, or 128 for the pitch bend; value 0-1 (bend: 0.5 is centre)
};

struct Event {
    EventKind kind = EventKind::NoteOn;
    uint32_t frame = 0; // into the block, in order
    uint32_t id = 0;    // key or parameter id
    int32_t channel = 0;
    double value = 0.0;
};

// A parameter change through a ring: the app's knobs to the plugin, or the plugin's own changes
// (its interface, its output events) back to the app
enum class ChangeKind : uint32_t { Value, GestureBegin, GestureEnd };

struct ParamChange {
    ChangeKind kind = ChangeKind::Value;
    uint32_t id = 0;
    double value = 0.0;
};

// A single-producer, single-consumer ring that lives in shared memory. Like env::SpscQueue, but
// every index is masked where it is used, so a ring whose indices the other process scribbled on
// can't make this one read or write outside it.
template <typename T, uint32_t Capacity>
struct SharedRing {
    static_assert(Capacity >= 2 && (Capacity & (Capacity - 1)) == 0, "Capacity must be a power of two");
    static constexpr uint32_t kMask = Capacity - 1;

    void reset()
    {
        head.store(0, std::memory_order_relaxed);
        tail.store(0, std::memory_order_relaxed);
    }

    bool push(const T &item)
    {
        const uint32_t h = head.load(std::memory_order_relaxed) & kMask;
        const uint32_t next = (h + 1) & kMask;
        if (next == (tail.load(std::memory_order_acquire) & kMask))
            return false;
        items[h] = item;
        head.store(next, std::memory_order_release);
        return true;
    }

    bool pop(T &item)
    {
        const uint32_t t = tail.load(std::memory_order_relaxed) & kMask;
        if (t == (head.load(std::memory_order_acquire) & kMask))
            return false;
        item = items[t];
        tail.store((t + 1) & kMask, std::memory_order_release);
        return true;
    }

    T items[Capacity];
    alignas(64) std::atomic<uint32_t> head{0};
    alignas(64) std::atomic<uint32_t> tail{0};
};

// The transport as the plugin is told it, for the block
struct TransportInfo {
    uint32_t playing = 0;
    uint32_t beatsPerBar = 4; // the signature's numerator
    uint32_t beatUnit = 4;    // and its denominator
    int32_t bar = 0;          // the bar the block starts in, from 0
    double tempo = 120.0;
    double tempoPerSample = 0.0; // how the tempo changes over the block (in a ramp)
    double beats = 0.0;   // position, from the timeline's start
    double barStart = 0.0; // that bar's start, in beats
    double seconds = 0.0;
};

// The shared memory block of one instance. The app makes it (zeroed, then `magic` and `protocol`
// set); the host maps the same pages.
struct alignas(64) Shared {
    uint32_t magic;
    uint32_t protocol;

    // The audio channel. The app writes everything up to `events` and then `request`; the host
    // writes `output`, `status` and then `done`.
    alignas(64) std::atomic<uint32_t> request; // sequence number of the latest request
    alignas(64) std::atomic<uint32_t> done;    // the request the host last finished
    uint32_t frames;
    uint32_t inputChannels;
    uint32_t eventCount;
    TransportInfo transport;
    Event events[kMaxBlockEvents];
    int32_t status; // the plugin's process status for the block (CLAP_PROCESS_*)
    uint32_t outputChannels;
    // The sidechain key for the block (an effect keyed by another track): `keyChannels` of it are
    // written (0: no key this block; the host then gives the plugin's sidechain port silence)
    uint32_t keyChannels;
    float key[kMaxChannels][kMaxFrames];
    float input[kMaxChannels][kMaxFrames];
    float output[kMaxChannels][kMaxFrames];

    // Parameter changes: the app's main thread to the plugin (applied at the start of the next
    // block, or flushed while the plugin isn't processing), and the plugin's back to the app's
    // main thread
    SharedRing<ParamChange, 1024> toPlugin;
    SharedRing<ParamChange, 1024> fromPlugin;

    // Written by the host: the plugin's latency in frames, and flags the app picks up
    std::atomic<uint32_t> latency;
};

// Control messages. Requests carry a sequence number the reply repeats; the host's own news
// (logs, rescans) has number 0.
enum class Message : uint32_t {
    // Requests
    Scan = 1,   // path -> Reply: descriptors
    Load,       // path, plugin id -> Reply: name, ports, extensions
    Activate,   // sample rate, max frames -> Reply
    Deactivate, // -> Reply
    Params,     // -> Reply: every parameter's info and value
    ParamText,  // id, value -> Reply: text
    SaveState,  // -> Reply: bytes
    LoadState,  // bytes -> Reply
    Quit,       // no reply; the host exits
    Ping,       // -> Reply
    GuiOpen,    // -> Reply: the plugin's own window is showing
    GuiClose,   // -> Reply
    GuiEmbed,   // frame mapping handle (in the host) -> Reply: width, height, the frame window
                // (which the app places). Its interface drawn into the GuiFrame for the app to show
    GuiInput,   // a GuiInput: no reply
    GuiResize,  // width, height: no reply
    GuiClip,    // x, y, width, height in the interface's pixels: the part shown (and taking the
                // mouse); a zero width for all of it. No reply

    Probe,      // path, plugin id -> Reply: the channels of its sidechain input port (0 for none). Makes and
                // initialises the plugin, so a plugin that crashes there costs only this request's host

    // Replies
    Reply = 100,
    Fail, // text

    // News from the host
    Log = 200,      // severity, text
    ParamsChanged,  // rescan flags: the parameter list or values changed
    LatencyChanged, // frames
    StateDirty,     // the plugin's state changed in a way parameters don't show
    GuiClosed,      // the user closed the plugin's window
};

struct MessageHeader {
    uint32_t type;
    uint32_t sequence;
    uint32_t size; // bytes of payload after the header
};

// Builds a payload
struct Writer {
    std::string data;

    void u32(uint32_t v) { raw(&v, sizeof v); }
    void f64(double v) { raw(&v, sizeof v); }
    void str(const std::string &s)
    {
        u32(static_cast<uint32_t>(s.size()));
        data.append(s);
    }
    void raw(const void *p, size_t n) { data.append(static_cast<const char *>(p), n); }
};

// Reads a payload; after a short read every value is zero or empty and ok() is false
struct Reader {
    Reader(const char *data, size_t size) : p_(data), left_(size) {}
    explicit Reader(const std::string &s) : Reader(s.data(), s.size()) {}

    uint32_t u32()
    {
        uint32_t v = 0;
        raw(&v, sizeof v);
        return v;
    }
    double f64()
    {
        double v = 0.0;
        raw(&v, sizeof v);
        return v;
    }
    std::string str()
    {
        const uint32_t n = u32();
        if (!ok_ || n > left_) {
            ok_ = false;
            return {};
        }
        std::string s(p_, n);
        p_ += n;
        left_ -= n;
        return s;
    }
    bool ok() const { return ok_; }

private:
    void raw(void *out, size_t n)
    {
        if (!ok_ || n > left_) {
            ok_ = false;
            std::memset(out, 0, n);
            return;
        }
        std::memcpy(out, p_, n);
        p_ += n;
        left_ -= n;
    }

    const char *p_;
    size_t left_;
    bool ok_ = true;
};

// What a .clap file holds, from a scan
struct Descriptor {
    std::string id;
    std::string name;
    std::string vendor;
    std::string version;
    std::string description;
    std::string url;
    std::string features; // CLAP feature strings, space-separated ("instrument synthesizer stereo")
    std::string path;     // the .clap file (filled by the app)
    // Channels (1 or 2) of its sidechain input, the first audio input port that isn't the main one; 0
    // for none. Learnt by a Probe after the scan (never on the wire in a scan's reply), and kept in
    // the scan cache with `probed`, whether a probe was made (a failed one leaves 0, not tried again)
    uint32_t sidechain = 0;
    bool probed = false;

    bool instrument() const { return hasFeature("instrument"); }
    bool effect() const { return hasFeature("audio-effect") || !instrument(); }
    bool hasFeature(const std::string &f) const { return (" " + features + " ").find(" " + f + " ") != std::string::npos; }
};

inline void writeDescriptor(Writer &w, const Descriptor &d)
{
    w.str(d.id);
    w.str(d.name);
    w.str(d.vendor);
    w.str(d.version);
    w.str(d.description);
    w.str(d.url);
    w.str(d.features);
}

inline Descriptor readDescriptor(Reader &r)
{
    Descriptor d;
    d.id = r.str();
    d.name = r.str();
    d.vendor = r.str();
    d.version = r.str();
    d.description = r.str();
    d.url = r.str();
    d.features = r.str();
    return d;
}

// One of a plugin's parameters, as the app shows it
struct ParamInfo {
    uint32_t id = 0;
    uint32_t flags = 0; // CLAP_PARAM_* flags
    std::string name;
    std::string module; // "Filter/Envelope"; may be empty
    double min = 0.0;
    double max = 1.0;
    double def = 0.0;
    double value = 0.0;

    bool stepped() const { return flags & 1u; }       // CLAP_PARAM_IS_STEPPED
    bool hidden() const { return flags & (1u << 2); } // CLAP_PARAM_IS_HIDDEN
    bool readOnly() const { return flags & (1u << 3); }
    bool bypass() const { return flags & (1u << 4); }
};

inline void writeParam(Writer &w, const ParamInfo &p)
{
    w.u32(p.id);
    w.u32(p.flags);
    w.str(p.name);
    w.str(p.module);
    w.f64(p.min);
    w.f64(p.max);
    w.f64(p.def);
    w.f64(p.value);
}

inline ParamInfo readParam(Reader &r)
{
    ParamInfo p;
    p.id = r.u32();
    p.flags = r.u32();
    p.name = r.str();
    p.module = r.str();
    p.min = r.f64();
    p.max = r.f64();
    p.def = r.f64();
    p.value = r.f64();
    return p;
}

// What Load tells the app about the plugin it made
struct LoadedInfo {
    std::string name;
    uint32_t inputChannels = 0;  // of its main audio input (0 for none)
    uint32_t outputChannels = 0; // of its main audio output
    uint32_t notePorts = 0;      // note inputs
    uint32_t latency = 0;
    bool params = false;
    bool state = false;
    bool gui = false;      // it has a window of its own it can open (CLAP's floating win32 interface)
    bool embedded = false; // its interface can be drawn into the app (CLAP's embedded win32 interface)
};

// A plugin's embedded interface, as the host captures it: the latest picture of its window, under
// a sequence lock (odd while the host writes; the app copies only between two equal, even reads).
constexpr uint32_t kGuiMaxWidth = 2560;
constexpr uint32_t kGuiMaxHeight = 1600;
struct GuiFrame {
    uint32_t magic;
    std::atomic<uint32_t> sequence;
    uint32_t width;
    uint32_t height;
    uint8_t pixels[kGuiMaxWidth * kGuiMaxHeight * 4]; // blue, green, red, unused; rows packed
};

// Mouse and keys for an embedded interface, in its pixels
enum class GuiInputKind : uint32_t { Move, Down, Up, DoubleClick, Wheel, Leave, KeyDown, KeyUp, Char };
struct GuiInput {
    GuiInputKind kind = GuiInputKind::Move;
    int32_t x = 0;
    int32_t y = 0;
    uint32_t button = 0; // 0 left, 1 right, 2 middle
    uint32_t mods = 0;   // 1 shift, 2 ctrl, 4 alt
    int32_t wheel = 0;   // 120 a notch, up positive
    uint32_t key = 0;    // a Windows virtual key, or a character for Char
};

}
