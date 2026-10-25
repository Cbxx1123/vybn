#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "Device.h"
#include "Effects.h"

namespace audio {

// Live MIDI effects (docs/design/midi-effects-plan.md): devices before a track's instrument that turn
// the notes it is given (from its clips and from keyboards) into others. They run on the audio thread
// inside ChainInstrument, a block at a time, and never lock or allocate: every list is a fixed array.

enum class MidiEffectType { Arpeggiator, Chord, Scale, Echo, Velocity };
constexpr int kMidiEffectTypeCount = 5;
constexpr size_t kMaxMidiEffects = 8; // in one chain

// A built-in MIDI effect's description: its name, key (the device id in project files) and parameters
// (ids 0, 1, ... as listed)
struct MidiEffectInfo {
    const char *name;
    const char *key;
    size_t paramCount;
    std::array<ParamInfo, kMaxEffectParams> params;
};
const MidiEffectInfo &midiEffectInfo(MidiEffectType type);
bool midiEffectFromKey(const std::string &key, MidiEffectType &type);
// A MIDI effect's setting with every parameter at its default (DeviceKind::MidiEffect)
DeviceSetting midiEffectSetting(MidiEffectType type);

// A note event in a block: its frame, on or off, key, and velocity (0-1, for an on)
struct MidiEvent {
    uint32_t frame = 0;
    bool on = false;
    int key = 60;
    float velocity = 0.8f;
};

// A block's events, in a fixed array
struct MidiEventList {
    static constexpr size_t kCapacity = 512;
    std::array<MidiEvent, kCapacity> events{};
    size_t count = 0;

    void clear() { count = 0; }
    void push(const MidiEvent &e)
    {
        if (count < kCapacity)
            events[count++] = e;
    }
    // In frame order, offs before ons on one frame (so a repeated key restarts)
    void sort();
};

// Where a block is in time: its first frame's absolute sample (the transport's position while it plays,
// else a free-running count), and its beats (the song's while it plays, else a free-running beat at the
// transport's tempo), taken as straight within a block
struct MidiClock {
    uint32_t frames = 0;
    double sampleRate = 48000.0;
    uint64_t start = 0;
    double beat0 = 0.0;
    double beatsPerFrame = 0.0;

    double beatAt(double frame) const { return beat0 + frame * beatsPerFrame; }
    double frameAt(double beat) const { return beatsPerFrame > 0.0 ? (beat - beat0) / beatsPerFrame : 0.0; }
    // Frames a span of beats lasts
    double framesFor(double beats) const { return beatsPerFrame > 0.0 ? beats / beatsPerFrame : 0.0; }
};

// A MIDI effect: a Device (parameters, settings) that processes note events. Its parameters are atomics,
// set from any thread; process runs on the audio thread.
class MidiEffect : public Device {
public:
    explicit MidiEffect(MidiEffectType type);

    MidiEffectType type() const { return type_; }
    DeviceKind kind() const override { return DeviceKind::MidiEffect; }
    bool matches(const DeviceSetting &setting) const override;
    void applySetting(const DeviceSetting &setting) override;
    std::vector<DeviceParam> params() const override;

    void setParam(size_t index, float value);
    float param(size_t index) const { return values_[index].load(std::memory_order_relaxed); }
    void setBypass(bool bypass) { bypass_.store(bypass, std::memory_order_relaxed); }
    bool bypassed() const { return bypass_.load(std::memory_order_relaxed); }

    // Audio thread: a block's events through the effect (`in` sorted), appended to `out`. Bypassed it
    // passes them on, first ending the notes it had made.
    void run(const MidiEventList &in, MidiEventList &out, const MidiClock &clock);
    // Audio thread: forgets every note and everything waiting (the instrument is told all notes off)
    void reset();

protected:
    virtual void process(const MidiEventList &in, MidiEventList &out, const MidiClock &clock) = 0;
    // Note-offs for every note the effect has sounding, at `frame`, and its state forgotten
    virtual void release(MidiEventList &out, uint32_t frame) = 0;

    // Events for later blocks, in absolute samples
    struct Pending {
        uint64_t at = 0;
        bool on = false;
        int key = 0;
        float velocity = 0.0f;
    };
    void schedule(uint64_t at, bool on, int key, float velocity);
    // The waiting events that fall in this block, into `out`
    void due(MidiEventList &out, const MidiClock &clock);
    void dropPending() { pendingCount_ = 0; }
    // Counted output keys: emits an on (counting it) and an off only when the last of a key's ons ends
    void noteOn(MidiEventList &out, uint32_t frame, int key, float velocity);
    void noteOff(MidiEventList &out, uint32_t frame, int key);
    void releaseCounted(MidiEventList &out, uint32_t frame);

private:
    MidiEffectType type_;
    std::array<std::atomic<float>, kMaxEffectParams> values_{};
    std::atomic<bool> bypass_{false};
    bool wasBypassed_ = false;
    static constexpr size_t kMaxPending = 512;
    std::array<Pending, kMaxPending> pending_{};
    size_t pendingCount_ = 0;
    std::array<uint8_t, 128> counts_{};
};

// Makes the MIDI effect for a setting (an unknown id gives a Velocity effect at its defaults)
std::unique_ptr<MidiEffect> makeMidiEffect(const DeviceSetting &setting);

// A stable pseudo-random number in 0-1 from two numbers (the arpeggiator's random steps, velocity's
// random amount), so a part plays the same every time
float midiHash(uint64_t a, uint64_t b);

}
