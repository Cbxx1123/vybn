// The host's CLAP plugins. The host implements only these CLAP host extensions: log, thread-check,
// params, audio-ports, note-ports, state, latency and gui; a plugin asking for anything else gets
// nothing.

#include "plugin/host/Host.h"

#include <cstring>
#include <mutex>

#include <clap/clap.h>

namespace host {

namespace {

using namespace plugin;

std::atomic<bool> callbackRequested{false};
std::atomic<bool> flushRequested{false};

// --- Host extensions

void hostLog(const clap_host *, clap_log_severity severity, const char *message)
{
    log(severity, message ? message : "");
}

const clap_host_log kLog = {hostLog};
const clap_host_thread_check kThreadCheck = {[](const clap_host *) { return isMainThread(); },
                                             [](const clap_host *) { return isAudioThread(); }};
const clap_host_params kParams = {
    [](const clap_host *, clap_param_rescan_flags flags) { paramsRescan.fetch_or(flags | 0x80000000u); },
    [](const clap_host *, clap_id, clap_param_clear_flags) {},
    [](const clap_host *) {
        flushRequested = true;
        wake();
    },
};
const clap_host_audio_ports kAudioPorts = {
    [](const clap_host *, uint32_t) { return false; },
    [](const clap_host *, uint32_t) {
        restartRequested = true;
        wake();
    },
};
const clap_host_note_ports kNotePorts = {
    [](const clap_host *) -> uint32_t { return CLAP_NOTE_DIALECT_CLAP | CLAP_NOTE_DIALECT_MIDI; },
    [](const clap_host *, uint32_t) {},
};
const clap_host_state kState = {[](const clap_host *) { stateDirty = true; }};
const clap_host_latency kLatency = {[](const clap_host *) { latencyChanged = true; }};

const clap_host_gui kGui = {
    [](const clap_host *) {},
    // A floating window sizes itself; an embedded one asks for its frame to grow or shrink
    [](const clap_host *, uint32_t w, uint32_t h) { return resizeFrame(w, h); },
    [](const clap_host *) { return false; },
    [](const clap_host *) { return false; },
    [](const clap_host *, bool) {
        guiClosed = true;
        wake();
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
    "VYBN",
    "VYBN",
    "",
    "1.0",
    hostExtension,
    [](const clap_host *) {
        restartRequested = true;
        wake();
    },
    [](const clap_host *) {},
    [](const clap_host *) {
        callbackRequested = true;
        wake();
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

struct Port {
    clap_audio_buffer buffer{};
    std::vector<float *> channels;
    std::vector<std::vector<float>> scratch; // for ports that aren't the main one, and extra channels
};

constexpr int kNoPort = -1;

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

class ClapFormat : public Format {
public:
    bool open(const std::string &path, std::string &error) override
    {
        if (module_)
            return true;
        // Only the plugin's own folder and the system's are searched for its DLLs
        module_ = LoadLibraryExW(widen(path).c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        if (!module_) {
            error = "Couldn't load the plugin's file (error " + std::to_string(GetLastError()) + ")";
            return false;
        }
        entry_ = reinterpret_cast<const clap_plugin_entry *>(GetProcAddress(module_, "clap_entry"));
        if (!entry_) {
            error = "Not a CLAP plugin: it has no clap_entry";
            return false;
        }
        if (!clap_version_is_compatible(entry_->clap_version)) {
            error = "The plugin was built for an incompatible CLAP version";
            return false;
        }
        if (!entry_->init(path.c_str())) {
            error = "The plugin refused to start (its entry's init failed)";
            return false;
        }
        entryReady_ = true;
        factory_ = static_cast<const clap_plugin_factory *>(entry_->get_factory(CLAP_PLUGIN_FACTORY_ID));
        if (!factory_) {
            error = "The plugin has no plugin factory";
            return false;
        }
        return true;
    }

    bool scan(std::vector<Descriptor> &found, std::string &) override
    {
        const uint32_t count = factory_->get_plugin_count(factory_);
        for (uint32_t i = 0; i < count && i < 1000; ++i) {
            const clap_plugin_descriptor *d = factory_->get_plugin_descriptor(factory_, i);
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
        return true;
    }

    bool probe(const std::string &id, uint32_t &sidechain, std::string &error) override
    {
        const clap_plugin *p = factory_->create_plugin(factory_, &kHost, id.c_str());
        if (!p) {
            error = "The plugin file has no plugin \"" + id + "\"";
            return false;
        }
        sidechain = p->init(p) ? sidechainChannels(p) : 0;
        p->destroy(p);
        return true;
    }

    bool load(const std::string &id, LoadedInfo &info, std::string &error) override
    {
        const clap_plugin *p = factory_->create_plugin(factory_, &kHost, id.c_str());
        if (!p) {
            error = "The plugin file has no plugin \"" + id + "\"";
            return false;
        }
        if (!p->init(p)) {
            p->destroy(p);
            error = "The plugin failed to start (init)";
            return false;
        }
        plugin_ = p;
        params_ = static_cast<const clap_plugin_params *>(p->get_extension(p, CLAP_EXT_PARAMS));
        audioPorts_ = static_cast<const clap_plugin_audio_ports *>(p->get_extension(p, CLAP_EXT_AUDIO_PORTS));
        notePorts_ = static_cast<const clap_plugin_note_ports *>(p->get_extension(p, CLAP_EXT_NOTE_PORTS));
        state_ = static_cast<const clap_plugin_state *>(p->get_extension(p, CLAP_EXT_STATE));
        latency_ = static_cast<const clap_plugin_latency *>(p->get_extension(p, CLAP_EXT_LATENCY));
        gui_ = static_cast<const clap_plugin_gui *>(p->get_extension(p, CLAP_EXT_GUI));

        info.name = p->desc && p->desc->name ? p->desc->name : id;
        if (audioPorts_) {
            for (int input = 1; input >= 0; --input) {
                const uint32_t n = audioPorts_->count(p, input != 0);
                for (uint32_t i = 0; i < n; ++i) {
                    clap_audio_port_info port{};
                    if (audioPorts_->get(p, i, input != 0, &port) && (port.flags & CLAP_AUDIO_PORT_IS_MAIN || i == 0)) {
                        (input ? info.inputChannels : info.outputChannels) = port.channel_count;
                        if (port.flags & CLAP_AUDIO_PORT_IS_MAIN)
                            break;
                    }
                }
            }
        }
        if (notePorts_) {
            info.notePorts = notePorts_->count(p, true);
            clap_note_port_info port{};
            if (info.notePorts > 0 && notePorts_->get(p, 0, true, &port)) {
                if (port.supported_dialects & CLAP_NOTE_DIALECT_CLAP)
                    noteDialect_ = CLAP_NOTE_DIALECT_CLAP;
                else if (port.supported_dialects & CLAP_NOTE_DIALECT_MIDI)
                    noteDialect_ = CLAP_NOTE_DIALECT_MIDI;
                midiControllers_ = (port.supported_dialects & CLAP_NOTE_DIALECT_MIDI) != 0;
            }
        }
        info.params = params_ != nullptr;
        info.state = state_ != nullptr;
        info.gui = gui_ && gui_->is_api_supported(p, CLAP_WINDOW_API_WIN32, true);
        info.embedded = gui_ && gui_->is_api_supported(p, CLAP_WINDOW_API_WIN32, false);
        return true;
    }

    bool loaded() const override { return plugin_ != nullptr; }

    bool activate(double rate, std::string &error) override
    {
        buildPorts();
        if (!plugin_->activate(plugin_, rate, 1, kMaxFrames)) {
            error = "The plugin refused to activate";
            return false;
        }
        shared->latency.store(latency());
        return true;
    }

    void deactivate() override { plugin_->deactivate(plugin_); }

    uint32_t latency() override { return latency_ ? latency_->get(plugin_) : 0; }

    std::vector<ParamInfo> params() override
    {
        std::vector<ParamInfo> list;
        if (!params_)
            return list;
        const uint32_t n = params_->count(plugin_);
        for (uint32_t i = 0; i < n && i < 10000; ++i) {
            clap_param_info info{};
            if (!params_->get_info(plugin_, i, &info))
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
            p.value = params_->get_value(plugin_, info.id, &value) ? value : info.default_value;
            list.push_back(p);
        }
        return list;
    }

    bool paramText(uint32_t id, double value, std::string &text) override
    {
        char buffer[256] = {};
        if (!params_ || !params_->value_to_text(plugin_, id, value, buffer, sizeof buffer))
            return false;
        buffer[sizeof buffer - 1] = 0;
        text = buffer;
        return true;
    }

    bool hasState() const override { return state_ != nullptr; }

    bool saveState(std::string &out, std::string &error) override
    {
        OutStream stream;
        if (!state_->save(plugin_, &stream.stream)) {
            error = "The plugin couldn't save its state";
            return false;
        }
        out = std::move(stream.data);
        return true;
    }

    bool loadState(const std::string &in, std::string &error) override
    {
        InStream stream;
        stream.data = &in;
        if (!state_->load(plugin_, &stream.stream)) {
            error = "The plugin couldn't load the state";
            return false;
        }
        return true;
    }

    bool guiEmbed(HWND frame, std::string &error) override
    {
        if (!gui_) {
            error = "This plugin has no interface";
            return false;
        }
        if (!gui_->is_api_supported(plugin_, CLAP_WINDOW_API_WIN32, false)) {
            error = "This plugin's interface only opens as a window of its own";
            return false;
        }
        if (!gui_->create(plugin_, CLAP_WINDOW_API_WIN32, false)) {
            error = "The plugin couldn't make its interface";
            return false;
        }
        guiCreated_ = true;
        gui_->set_scale(plugin_, 1.0);
        uint32_t width = 640, height = 400;
        if (!gui_->get_size(plugin_, &width, &height) || width == 0 || height == 0)
            width = 640, height = 400;
        resizeFrame(width, height);
        clap_window window{};
        window.api = CLAP_WINDOW_API_WIN32;
        window.win32 = frame;
        if (!gui_->set_parent(plugin_, &window)) {
            error = "The plugin couldn't place its interface";
            return false;
        }
        gui_->show(plugin_);
        return true;
    }

    void guiResize(uint32_t width, uint32_t height) override
    {
        if (!guiCreated_ || !gui_->can_resize(plugin_))
            return;
        gui_->adjust_size(plugin_, &width, &height);
        if (gui_->set_size(plugin_, width, height))
            resizeFrame(width, height);
    }

    bool guiOpen(std::string &error) override
    {
        if (!gui_) {
            error = "This plugin has no window of its own";
            return false;
        }
        if (!guiCreated_) {
            if (!gui_->is_api_supported(plugin_, CLAP_WINDOW_API_WIN32, true)) {
                error = "This plugin's interface only opens inside a host's window, which VYBN doesn't offer yet";
                return false;
            }
            if (!gui_->create(plugin_, CLAP_WINDOW_API_WIN32, true)) {
                error = "The plugin couldn't make its window";
                return false;
            }
            guiCreated_ = true;
            if (plugin_->desc && plugin_->desc->name)
                gui_->suggest_title(plugin_, plugin_->desc->name);
        }
        if (!gui_->show(plugin_)) {
            error = "The plugin couldn't show its window";
            return false;
        }
        return true;
    }

    void guiClose() override
    {
        if (guiCreated_) {
            gui_->hide(plugin_);
            gui_->destroy(plugin_);
            guiCreated_ = false;
        }
    }

    // Passes on what the plugin's threads asked for, and flushes parameters while it isn't processing
    void housekeeping() override
    {
        if (!plugin_)
            return;
        if (callbackRequested.exchange(false))
            plugin_->on_main_thread(plugin_);
        // While inactive the app's changes reach the plugin through flush
        const bool flush = flushRequested.exchange(false);
        if (!pluginActive() && params_) {
            EventList ev;
            ParamChange change;
            while (shared->toPlugin.pop(change))
                if (change.kind == ChangeKind::Value)
                    ev.addParam(0, change.id, change.value);
            if (ev.count > 0 || flush)
                params_->flush(plugin_, &ev.in, &kOutEvents);
        }
    }

    void unload() override
    {
        if (plugin_) {
            plugin_->destroy(plugin_);
            plugin_ = nullptr;
        }
        if (entryReady_)
            entry_->deinit();
        entryReady_ = false;
    }

    void process(uint32_t frames) override
    {
        if (mainOutput_ < 0) {
            silence(frames);
            shared->status = CLAP_PROCESS_SLEEP;
            return;
        }
        if (!processing_) {
            processing_ = plugin_->start_processing(plugin_);
            if (!processing_) {
                silence(frames);
                shared->status = CLAP_PROCESS_ERROR;
                return;
            }
        }

        // Events: the app's parameter changes first, then the block's own, in time order
        EventList &ev = blockEvents_;
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
                if (noteDialect_ == CLAP_NOTE_DIALECT_CLAP)
                    ev.addNote(time, on ? CLAP_EVENT_NOTE_ON : CLAP_EVENT_NOTE_OFF, key, channel, e.value);
                else if (noteDialect_ == CLAP_NOTE_DIALECT_MIDI) {
                    int velocity = static_cast<int>(e.value * 127.0 + 0.5);
                    velocity = velocity < 1 ? 1 : (velocity > 127 ? 127 : velocity);
                    ev.addMidi(time, static_cast<uint8_t>((on ? 0x90 : 0x80) | channel), static_cast<uint8_t>(key),
                               static_cast<uint8_t>(on ? velocity : 64));
                }
                break;
            }
            case EventKind::Controller: {
                if (!midiControllers_)
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
                if (noteDialect_ == CLAP_NOTE_DIALECT_CLAP)
                    ev.addNote(time, CLAP_EVENT_NOTE_OFF, -1, -1, 0.0);
                else if (noteDialect_ == CLAP_NOTE_DIALECT_MIDI)
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
        transport.tempo_inc = t.tempoPerSample > -1.0 && t.tempoPerSample < 1.0 ? t.tempoPerSample : 0.0;
        transport.song_pos_beats = static_cast<clap_beattime>(t.beats * CLAP_BEATTIME_FACTOR);
        transport.song_pos_seconds = static_cast<clap_sectime>(t.seconds * CLAP_SECTIME_FACTOR);
        transport.tsig_num = static_cast<uint16_t>(t.beatsPerBar > 0 && t.beatsPerBar < 64 ? t.beatsPerBar : 4);
        transport.tsig_denom = static_cast<uint16_t>(t.beatUnit > 0 && t.beatUnit <= 32 ? t.beatUnit : 4);
        transport.bar_number = t.bar;
        transport.bar_start = static_cast<clap_beattime>(t.barStart * CLAP_BEATTIME_FACTOR);

        // Audio: the main ports read and write the shared block
        if (mainInput_ >= 0) {
            Port &in = inputs_[static_cast<size_t>(mainInput_)];
            const uint32_t given = shared->inputChannels <= kMaxChannels ? shared->inputChannels : kMaxChannels;
            for (uint32_t c = 0; c < in.channels.size() && c < kMaxChannels; ++c)
                in.channels[c] = given > 0 ? shared->input[c < given ? c : given - 1] : in.scratch[c].data();
        }
        // The sidechain port: the app's key when there is one, else its own silent buffers (a port must
        // always have buffers)
        if (sidechainInput_ >= 0) {
            Port &side = inputs_[static_cast<size_t>(sidechainInput_)];
            const uint32_t given = shared->keyChannels <= kMaxChannels ? shared->keyChannels : kMaxChannels;
            for (uint32_t c = 0; c < side.channels.size() && c < kMaxChannels; ++c)
                side.channels[c] = given > 0 ? shared->key[c < given ? c : given - 1] : side.scratch[c].data();
        }
        Port &out = outputs_[static_cast<size_t>(mainOutput_)];
        for (uint32_t c = 0; c < out.channels.size() && c < kMaxChannels; ++c)
            out.channels[c] = shared->output[c];

        clap_process process{};
        process.steady_time = steady_;
        steady_ += frames;
        process.frames_count = frames;
        process.transport = &transport;
        process.audio_inputs = inBuffers_.data();
        process.audio_inputs_count = static_cast<uint32_t>(inBuffers_.size());
        process.audio_outputs = outBuffers_.data();
        process.audio_outputs_count = static_cast<uint32_t>(outBuffers_.size());
        process.in_events = &ev.in;
        process.out_events = &kOutEvents;

        const clap_process_status status = plugin_->process(plugin_, &process);
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

    void stopProcessing() override
    {
        if (processing_) {
            plugin_->stop_processing(plugin_);
            processing_ = false;
        }
    }

private:
    // Builds the port buffers for the plugin's current ports
    void buildPorts()
    {
        const clap_plugin *p = plugin_;
        for (int input = 1; input >= 0; --input) {
            std::vector<Port> &ports = input ? inputs_ : outputs_;
            int &main = input ? mainInput_ : mainOutput_;
            ports.clear();
            main = -1;
            const uint32_t n = audioPorts_ ? audioPorts_->count(p, input != 0) : 0;
            for (uint32_t i = 0; i < n && i < 32; ++i) {
                clap_audio_port_info info{};
                if (!audioPorts_->get(p, i, input != 0, &info))
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
                sidechainInput_ = kNoPort;
                for (size_t i = 0; i < ports.size(); ++i) {
                    if (static_cast<int>(i) != main && (ports[i].scratch.size() == 1 || ports[i].scratch.size() == 2)) {
                        sidechainInput_ = static_cast<int>(i);
                        break;
                    }
                }
            }
            std::vector<clap_audio_buffer> &buffers = input ? inBuffers_ : outBuffers_;
            buffers.clear();
            for (Port &port : ports) {
                port.buffer.data32 = port.channels.data();
                buffers.push_back(port.buffer);
            }
        }
    }

    HMODULE module_ = nullptr;
    const clap_plugin_entry *entry_ = nullptr;
    bool entryReady_ = false;
    const clap_plugin_factory *factory_ = nullptr;
    const clap_plugin *plugin_ = nullptr;
    const clap_plugin_params *params_ = nullptr;
    const clap_plugin_audio_ports *audioPorts_ = nullptr;
    const clap_plugin_note_ports *notePorts_ = nullptr;
    const clap_plugin_state *state_ = nullptr;
    const clap_plugin_latency *latency_ = nullptr;
    const clap_plugin_gui *gui_ = nullptr;
    bool guiCreated_ = false;
    uint32_t noteDialect_ = 0;      // what notes are sent as: CLAP_NOTE_DIALECT_CLAP or _MIDI, or 0 for none
    bool midiControllers_ = false; // the note port takes MIDI, so controllers and the bend go as MIDI

    // Audio thread
    bool processing_ = false;
    int64_t steady_ = 0;
    EventList blockEvents_;
    std::vector<Port> inputs_;
    std::vector<Port> outputs_;
    std::vector<clap_audio_buffer> inBuffers_; // one per port, pointing at its channels
    std::vector<clap_audio_buffer> outBuffers_;
    int mainInput_ = -1;
    int sidechainInput_ = -1; // the first input port that isn't the main one, with 1 or 2 channels: the key's
    int mainOutput_ = -1;
};

}

std::unique_ptr<Format> makeClapFormat() { return std::unique_ptr<Format>(new ClapFormat); }

}
