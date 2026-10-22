#pragma once

#include <array>
#include <memory>
#include <string>
#include <vector>

#include "Device.h"
#include "Instrument.h"
#include "Sampler.h"
#include "plugin/Slot.h"

namespace audio {

// The instrument devices: what plays an instrument track's notes, first in its chain.

// A CLAP plugin as an instrument (DeviceKind::Plugin): notes go to it with their frames (it takes
// timed events, so a NotePlayer hands it each block whole) and its output is added into the
// block. The plugin runs in its own sandboxed host through a plugin::Slot. Silent while it loads,
// once it has crashed or hung until it is reloaded, and without a PluginManager (tests).
class PluginInstrument : public InstrumentDevice {
public:
    PluginInstrument(plugin::PluginManager *manager, const DeviceSetting &setting);

    DeviceKind kind() const override { return DeviceKind::Plugin; }
    bool matches(const DeviceSetting &setting) const override;
    void applySetting(const DeviceSetting &setting) override;
    void update() override;
    std::vector<DeviceParam> params() const override;
    std::string valueText(const DeviceParam &param, double value) const override;
    plugin::Slot *slot() override { return slot_.get(); }
    uint32_t latency() const override;

    void prepare(double sampleRate, uint32_t maxFrames) override;
    void process(AudioBlock &block) override;

    bool timedEvents() const override { return true; }
    void noteOn(int key, float velocity) override { noteOnAt(key, velocity, 0); }
    void noteOff(int key) override { noteOffAt(key, 0); }
    void noteOnAt(int key, float velocity, uint32_t frame) override;
    void noteOffAt(int key, uint32_t frame) override;
    void allNotesOff() override;
    void controlChange(int controller, float value) override { controlChangeAt(controller, value, 0); }
    void controlChangeAt(int controller, float value, uint32_t frame) override;
    // A parameter value event at the start of the next block it processes
    bool automatable() const override { return true; }
    void automate(uint32_t paramId, double value) override;

private:
    bool push(plugin::EventKind kind, int key, float velocity, uint32_t frame);

    std::string id_;
    std::unique_ptr<plugin::Slot> slot_;
    // Audio thread: the block's events, and the plugin's output before it is added in
    std::array<plugin::Event, plugin::kMaxBlockEvents> events_{};
    uint32_t eventCount_ = 0;
    // Audio thread: how many notes each key has sounding in the plugin. All notes off releases
    // each of them by key: a wildcard note off (or MIDI's all notes off) isn't heeded by every
    // plugin (Surge XT's last note kept sounding after the transport stopped)
    std::array<uint8_t, 128> held_{};
    std::vector<float> scratch_[2];
};

// A library instrument (DeviceKind::Sampler, id the instrument's name) on a Sampler. The samples
// are loaded by the app (the library is the DAW's) and handed to sampler().setSampleSet.
class SamplerDevice : public InstrumentDevice {
public:
    explicit SamplerDevice(const DeviceSetting &setting);

    static constexpr uint32_t kLevel = 0; // dB
    static constexpr double kDefaultLevel = -6.0; // samples are near full scale and notes overlap
    // The setting for a library instrument, at its defaults
    static DeviceSetting setting(const std::string &instrument);

    Sampler &sampler() { return sampler_; }
    const std::string &instrumentName() const { return id_; }

    DeviceKind kind() const override { return DeviceKind::Sampler; }
    bool matches(const DeviceSetting &setting) const override;
    void applySetting(const DeviceSetting &setting) override;
    std::vector<DeviceParam> params() const override;
    bool automatable() const override { return true; }
    void automate(uint32_t paramId, double value) override;

    void prepare(double sampleRate, uint32_t maxFrames) override { sampler_.prepare(sampleRate, maxFrames); }
    void process(AudioBlock &block) override { sampler_.process(block); }
    void beginBlock() override { sampler_.beginBlock(); }
    void collect() override { sampler_.collect(); }
    void noteOn(int key, float velocity) override { sampler_.noteOn(key, velocity); }
    void noteOff(int key) override { sampler_.noteOff(key); }
    void allNotesOff() override { sampler_.allNotesOff(); }
    void controlChange(int controller, float value) override { sampler_.controlChange(controller, value); }

private:
    std::string id_;
    Sampler sampler_;
};

}
