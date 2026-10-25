#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <utility>
#include <vector>

#include "Automation.h"
#include "Device.h"
#include "Effects.h"
#include "Instrument.h"
#include "MidiEffects.h"
#include "Modulation.h"

namespace plugin {
class PluginManager;
}

namespace audio {

// The devices of one track (its instrument, then its effects) or of the master bus (effects
// only), made from the project's DeviceSettings. The main thread changes it with apply(); the
// audio thread runs it. A change builds a new immutable layout and swaps it in with one atomic
// store, so the audio thread sees the old chain or the new, never a mix; what it replaced is
// freed by collect() after the audio thread has finished a block or two. A device whose setting
// still matches (the same kind and id) is kept with its state: a reverb's tail, a plugin's host.
class DeviceChain {
public:
    DeviceChain();
    ~DeviceChain();

    DeviceChain(const DeviceChain &) = delete;
    DeviceChain &operator=(const DeviceChain &) = delete;

    // Main thread, before the chain is processed and when the device's format changes
    void prepare(double sampleRate, uint32_t maxFrames);
    // Main thread: where plugins come from (none: plugin effects pass the sound through, plugin
    // instruments are silent)
    void setPlugins(plugin::PluginManager *plugins) { plugins_ = plugins; }
    // Main thread: makes the chain match: `instrument` (DeviceKind::None for none) and `effects`
    // (at most kMaxEffects), then sets every value and bypass. Cheap when nothing changed.
    void apply(const DeviceSetting &instrument, const std::vector<DeviceSetting> &effects) { apply({}, instrument, effects); }
    void apply(const std::vector<DeviceSetting> &effects) { apply({}, DeviceSetting{}, effects); }
    // The same with the MIDI effects before the instrument (at most kMaxMidiEffects; MidiEffects.h)
    void apply(const std::vector<DeviceSetting> &midiEffects, const DeviceSetting &instrument,
               const std::vector<DeviceSetting> &effects);
    // Main thread: the automation it plays (Automation.h): each device's curves, by the device (one of
    // this chain's, as device() gives them; a device left out has none), and the track's own mix
    // curves (volume and pan, read by its TrackProcessor through automateMix). Swapped in like a new
    // layout. A device keeps its curves when apply() keeps it; a device made new has none until
    // this is called again.
    void setAutomation(const std::vector<std::pair<const Device *, AutomationCurves>> &devices, AutomationCurves mix);
    // Main thread: the track's modulators and their routes (Modulation.h): to its devices' parameters
    // (by the device, one of this chain's; slots index `modulators`) and to its volume and pan (`mix`).
    // Swapped in like automation; a modulator keeps its state while one with its id and kind stays.
    void setModulation(const std::vector<Modulator> &modulators,
                       const std::vector<std::pair<const Device *, ResolvedRoutes>> &devices, ResolvedRoutes mix);
    // Each modulator's last value, by its place in the list setModulation was given (0 for none)
    float modulatorValue(size_t slot) const;
    // Main thread: whether the chain renders offline (an export engine). Then every plugin in it is
    // waited for while it loads, rather than passing the sound through; devices made later inherit it.
    void setOffline(bool offline);
    // Main thread, once a frame: frees what apply() replaced, and lets each device catch up
    // (plugins take what their jobs finished)
    void collect();

    // Main thread
    InstrumentDevice *instrument() { return instrument_.get(); }
    size_t size() const { return effects_.size(); }
    Effect *effect(size_t index) { return index < effects_.size() ? effects_[index].get() : nullptr; }
    size_t midiEffectCount() const { return midiEffects_.size(); }
    MidiEffect *midiEffect(size_t index) { return index < midiEffects_.size() ? midiEffects_[index].get() : nullptr; }
    // The device at `index` (kInstrument: the instrument), or nullptr
    static constexpr int kInstrument = -1;
    Device *device(int index);
    // Frames the chain delays the sound by: its plugins' latencies
    uint32_t latency() const;

    // Audio thread: the instrument now (ChainInstrument plays it), and the effects over a block,
    // each set to its automation at the block's start first (with a transport)
    InstrumentDevice *liveInstrument() const;
    // Audio thread: the MIDI effects now, in order (ChainInstrument runs them); returns how many
    size_t liveMidiEffects(MidiEffect *const *&effects) const;
    void process(float *left, float *right, uint32_t frames, const TransportState *transport = nullptr);
    // Audio thread: the sidechain key for the effect at `effect`, for the next process only (the
    // pointers must stay good through it; process forgets every key when it is done, so a key the
    // routing drops never lingers into a later block). Effects without a sidechain ignore theirs.
    void setKey(size_t effect, const float *left, const float *right);
    // Audio thread: sets the instrument's automated parameters for the block starting at `transport`
    void automateInstrument(const TransportState &transport) const;
    // Audio thread: the track's volume (linear gain) and pan at the block's start, where they have
    // curves (and moved by their modulators); the others are left as they are
    void automateMix(const TransportState &transport, float &gain, float &pan) const;
    // Audio thread: another of the track's own lanes (a send's level) at the block's start, when it
    // has a curve; returns whether `value` was set
    bool automateMixParam(const TransportState &transport, uint32_t param, float &value) const;

private:
    // Immutable once published, so the audio thread reads it without a lock; the curves are shared
    // with the next layout (freed with the last that holds them, on the main thread)
    struct Layout {
        InstrumentDevice *instrument = nullptr;
        std::array<MidiEffect *, kMaxMidiEffects> midiEffects{};
        size_t midiCount = 0;
        std::array<Effect *, kMaxEffects> effects{};
        size_t count = 0;
        std::shared_ptr<const AutomationCurves> instrumentCurves;
        std::array<std::shared_ptr<const AutomationCurves>, kMaxEffects> effectCurves{};
        std::shared_ptr<const AutomationCurves> mixCurves;
        // Modulation: the modulators' states, and the routes of the instrument, each effect and the mix
        std::array<ModulatorState *, kMaxModulators> modulators{};
        size_t modulatorCount = 0;
        std::shared_ptr<const ResolvedRoutes> instrumentRoutes;
        std::array<std::shared_ptr<const ResolvedRoutes>, kMaxEffects> effectRoutes{};
        std::shared_ptr<const ResolvedRoutes> mixRoutes;
    };
    // Audio thread: this block's modulator values, worked out once a block (the first device to ask)
    void evaluate(const Layout &layout, const TransportState &transport) const;
    // Audio thread: a device's routed parameters set to their modulated values
    void modulate(Device &device, const ResolvedRoutes &routes, const AutomationCurves *curves, double beat) const;
    // Publishes a layout of the devices now, with their curves, and retires the old one
    void publish(uint64_t stamp);
    struct Retired {
        std::unique_ptr<Layout> layout;
        std::unique_ptr<Device> device;
        uint64_t stamp;
        std::unique_ptr<ModulatorState> modulator = nullptr;
    };

    void retire(std::unique_ptr<Device> device, uint64_t stamp);

    double sampleRate_ = 48000.0;
    uint32_t maxFrames_ = 4096;
    plugin::PluginManager *plugins_ = nullptr;
    bool offline_ = false;
    std::vector<std::unique_ptr<MidiEffect>> midiEffects_; // main thread, in order
    std::unique_ptr<InstrumentDevice> instrument_; // main thread
    std::vector<std::unique_ptr<Effect>> effects_; // main thread, in order
    // Main thread: each device's curves (only devices with some), and the mix's
    std::map<const Device *, std::shared_ptr<const AutomationCurves>> curves_;
    std::shared_ptr<const AutomationCurves> mixCurves_;
    // Main thread: the modulators (by id) and each device's routes, and the mix's
    std::vector<std::pair<uint32_t, std::unique_ptr<ModulatorState>>> modulators_;
    std::map<const Device *, std::shared_ptr<const ResolvedRoutes>> routes_;
    std::shared_ptr<const ResolvedRoutes> mixRoutes_;
    // Audio thread: this block's modulator values, the block they were worked out for, and the clock
    // while the transport is stopped
    mutable std::array<float, kMaxModulators> modValues_{};
    mutable uint64_t evaluated_ = ~0ull;
    double freeSeconds_ = 0.0;
    double freeBeat_ = 0.0;
    std::unique_ptr<Layout> layout_;               // what the audio thread runs
    std::atomic<const Layout *> published_{nullptr};
    std::vector<Retired> retired_;
    std::atomic<uint64_t> blocks_{0}; // blocks the audio thread has finished
    std::array<const float *, kMaxEffects> keyLeft_{};  // audio thread: this block's keys, by effect
    std::array<const float *, kMaxEffects> keyRight_{};
};

// Makes the device for a setting: a built-in effect, a sampler, or a plugin as an effect or an
// instrument (`plugins` may be null)
std::unique_ptr<Effect> makeEffectDevice(const DeviceSetting &setting, plugin::PluginManager *plugins);
std::unique_ptr<InstrumentDevice> makeInstrumentDevice(const DeviceSetting &setting, plugin::PluginManager *plugins);

// What a track's NotePlayer plays: whichever instrument its chain has now (silent without one).
// It reads the chain's layout at the start of each block, so a changed instrument takes over
// between blocks. The chain prepares and collects its devices; this only passes notes on.
class ChainInstrument : public Instrument {
public:
    ChainInstrument() : Instrument("Chain") {}
    // Main thread (the player may already be in the graph: the audio thread picks it up)
    void setChain(const DeviceChain *chain) { chain_.store(chain, std::memory_order_release); }

    void prepare(double, uint32_t) override {}
    void process(AudioBlock &block) override;
    void automateBlock(const TransportState &transport) override;
    void beginBlock() override;
    // With MIDI effects it takes every note with its frame and runs them over the block in process
    bool timedEvents() const override { return midiCount_ > 0 || (current_ && current_->timedEvents()); }
    void noteOn(int key, float velocity) override;
    void noteOff(int key) override;
    void noteOnAt(int key, float velocity, uint32_t frame) override;
    void noteOffAt(int key, uint32_t frame) override;
    void allNotesOff() override;
    void controlChange(int controller, float value) override;
    void controlChangeAt(int controller, float value, uint32_t frame) override;
    void noteExpressionAt(int key, int expression, float value, uint32_t frame) override;

private:
    // A note for the MIDI effects (this block's, in arrival order)
    void queue(bool on, int key, float velocity, uint32_t frame);
    // The instrument's part of a block, from frame `from` to `to`
    void render(AudioBlock &block, uint32_t from, uint32_t to);

    std::atomic<const DeviceChain *> chain_{nullptr};
    InstrumentDevice *current_ = nullptr; // audio thread: this block's
    // Audio thread: this block's MIDI effects, the notes for them, the lists they pass between them,
    // and the samples counted while the transport is stopped (their clock then)
    MidiEffect *const *midi_ = nullptr;
    size_t midiCount_ = 0;
    MidiEventList in_, a_, b_;
    uint64_t freeSamples_ = 0;
};

}
