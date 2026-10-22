#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

#include "Device.h"
#include "Instrument.h"
#include "SynthTables.h"

namespace audio {

// The built-in synthesizer's parameters. Their ids are saved in projects, so they never change.
// Percent-style rows are declared 0-100 (as the app shows them) and read by the engine as 0-1;
// millisecond rows are read in seconds; everything else is stored and read as shown.
namespace synth {

// Osc 1 (ids 0-8) and Osc 2 (ids 10-18): the same nine rows, at kOscBase[o] + offset
constexpr uint32_t kOscBase[2] = {0, 10};
constexpr uint32_t kOscWave = 0, kOscShape = 1, kOscLevel = 2, kOscOctave = 3, kOscSemitones = 4,
                   kOscFine = 5, kOscUnison = 6, kOscDetune = 7, kOscSpread = 8;

constexpr uint32_t kSubLevel = 20, kSubOctave = 21, kSubWave = 22, kNoiseLevel = 23, kNoiseColour = 24;

constexpr uint32_t kFilterType = 30, kCutoff = 31, kResonance = 32, kDrive = 33, kEnvAmount = 34,
                   kKeyTracking = 35, kFilterVelocity = 36;

// The amp envelope (40-44) and the filter envelope (50-53)
constexpr uint32_t kAmpAttack = 40, kAmpDecay = 41, kAmpSustain = 42, kAmpRelease = 43, kAmpVelocity = 44;
constexpr uint32_t kFilterAttack = 50, kFilterDecay = 51, kFilterSustain = 52, kFilterRelease = 53;

// LFO 1 (60-65) and LFO 2 (70-75): the same six rows, at kLfoBase[k] + offset
constexpr uint32_t kLfoBase[2] = {60, 70};
constexpr uint32_t kLfoWave = 0, kLfoRate = 1, kLfoSync = 2, kLfoSyncRate = 3, kLfoRetrigger = 4, kLfoFade = 5;

// Modulation slot k (0-5) at 80 + 3k (source), 81 + 3k (target), 82 + 3k (amount)
constexpr uint32_t kModSlotBase = 80;

constexpr uint32_t kVoiceMode = 100, kVoices = 101, kGlide = 102, kBendRange = 103, kDrift = 104, kVoiceLevel = 105;

// Ids index arrays of this size; unused ids are ignored
constexpr uint32_t kSynthParamSlots = 106;

constexpr int kMaxVoices = 16;
constexpr int kMaxUnison = 7;
// Envelopes, LFOs, the matrix and the filters update every this many frames
constexpr uint32_t kControlFrames = 16;

// Every parameter in the order the device shows them: the most useful first, then by id
std::vector<DeviceParam> synthParams();

// One cycle of an oscillator's wave at its Wave choice and Shape (0-1), for drawing: `count` samples
// over one cycle, as the engine reads the wave at its top level (level 0, no unison or detune, no
// level). Main thread only; `out` is written, never read.
void previewCycle(int wave, float shape, float *out, int count);

// The engine's parts, held by the Synth (named here so its helpers can work on them)

// One unison copy of an oscillator
struct Copy {
    double pos = 0.0;       // -1..1 across the unison: its detune and pan share this
    double phase = 0.0;     // cycles, 0-1
    double inc = 0.0;       // cycles per sample, moving toward the target across a control step
    double incStep = 0.0;   // the change in inc per sample
    double drift = 0.0;     // cents, a slow random walk
    double driftTarget = 0.0;
    double driftTime = 0.0; // seconds until the next target
    float gainL = 0.0f;
    float gainR = 0.0f;
};

// How an oscillator reads its wave this control step
struct OscRead {
    int mode = 0;              // 0 a skewed single cycle, 1 a pulse from a saw, 2 a table
    const float *a = nullptr;  // the frame (or the saw) at the mip level chosen for the note
    const float *b = nullptr;  // a table's second frame, or null
    float frac = 0.0f;         // between a and b
    double skew = 0.5;         // mode 0
    double width = 0.5;        // mode 1
};

struct Envelope {
    enum class Stage : uint8_t { Idle, Attack, Decay, Sustain, Release };
    Stage stage = Stage::Idle;
    float level = 0.0f;
};

struct Lfo {
    double phase = 0.0; // cycles, 0-1
    float held = 0.0f;  // random: the value of this cycle; smooth random: the next cycle's
    float heldPrev = 0.0f;
};

struct Voice {
    int key = -1;
    float velocity = 0.0f;
    uint64_t started = 0;   // for stealing the oldest
    float random = 0.0f;    // -1..1, the mod source Random, set per note
    double glide = 0.0;     // semitones still to slide, from the note it started from
    uint32_t seed = 1;      // this voice's random numbers (drift, LFO steps)
    uint32_t noise = 1;     // the noise generator
    bool fresh = true;      // the note just started: its oscillators take their pitch at once
    Envelope amp, filt;
    Lfo lfo[2];
    double lfoAge[2] = {0.0, 0.0};   // seconds since the note on (the LFOs' fade in)
    double lfoOct[2] = {0.0, 0.0};   // rate changes from the matrix, in octaves
    Copy copy[2][kMaxUnison];
    int count[2] = {1, 1};           // unison copies of each oscillator
    int sounding[2] = {0, 0};        // copies rendered (none while the oscillator's level is zero)
    OscRead read[2];
    OscRead subRead;
    double subPhase = 0.0, subInc = 0.0;
    float subGain = 0.0f;
    float noiseGain = 0.0f, noiseColour = 0.0f;
    float noiseLp[2] = {0.0f, 0.0f};
    float ampGain = 0.0f, ampTarget = 0.0f, ampStep = 0.0f;
    // The filter: a state-variable filter per channel and stage
    int filterType = 1;
    double a1 = 0.0, a2 = 0.0, a3 = 0.0, k = 2.0;
    float drive = 0.0f;
    double ic1[2][2] = {{0.0, 0.0}, {0.0, 0.0}};
    double ic2[2][2] = {{0.0, 0.0}, {0.0, 0.0}};
};

}

// The synthesizer: subtractive and wavetable voices (two oscillators with unison, a sub and noise,
// a state-variable filter, amp and filter envelopes, two LFOs and a six-slot modulation matrix),
// played as an instrument. Poly, Mono and Legato voicing with glide; the mod wheel, volume,
// expression, pitch bend, all notes off and reset are controllers.
//
// Threads: setParam and param are main thread; the rest is audio thread. Parameters are copied
// into the audio thread's array at the start of each block (beginBlock), so nothing is read from
// the main thread's atomics mid-block. Voices, copies and buffers are fixed-size members, so
// nothing is allocated on the audio thread.
class Synth : public Instrument {
public:
    explicit Synth(std::string name = "VYBN Synth");

    // Main thread: a parameter in its display units (a choice's index, percent-style rows 0-100,
    // ms, Hz...), clamped to its range. Unknown ids are ignored.
    void setParam(uint32_t id, double value);
    double param(uint32_t id) const;

    // Voices sounding now (for display and tests)
    int activeVoices() const { return active_.load(std::memory_order_relaxed); }
    // The rate (Hz) an LFO ran at in the last block: its own rate, or the transport's sync rate
    double lfoRateHz(int lfo) const { return lfoHz_[lfo]; }

    void prepare(double sampleRate, uint32_t maxFrames) override;
    void process(AudioBlock &block) override;

    void beginBlock() override;
    void noteOn(int key, float velocity) override;
    void noteOff(int key) override;
    void allNotesOff() override;
    void controlChange(int controller, float value) override;

private:
    void controlStep(uint32_t n);
    void updateVoice(synth::Voice &v, double dt, uint32_t n);
    void renderVoice(synth::Voice &v, uint32_t n);
    float runFilter(synth::Voice &v, int channel, float x);
    synth::Voice &pickVoice(int limit);
    void startVoice(synth::Voice &v, int key, float velocity, double glideSemis);
    void releaseVoice(synth::Voice &v);
    void pushHeld(int key, float velocity);
    bool removeHeld(int key);

    int mode() const;
    int voiceLimit() const;
    int unison(int osc) const;
    int lfoWave(int lfo) const;
    bool retrigger(int lfo) const { return p_[synth::kLfoBase[lfo] + synth::kLfoRetrigger] > 0.5f; }
    double baseRate(int lfo) const;

    // Main thread: what was set (display units), and the range and scale of each parameter
    std::array<std::atomic<float>, synth::kSynthParamSlots> stored_{};
    std::array<float, synth::kSynthParamSlots> low_{}, high_{}, scale_{};
    std::array<bool, synth::kSynthParamSlots> known_{};
    const synth::WaveBank *bank_ = nullptr;

    // Audio thread: parameters in engine units, copied at the start of each block
    std::array<float, synth::kSynthParamSlots> p_{};
    std::array<synth::Voice, synth::kMaxVoices> voices_{};
    struct Held {
        int key;
        float velocity;
    };
    std::array<Held, 16> held_{}; // the Mono and Legato keys held, the last played on top
    int heldCount_ = 0;
    int lastKey_ = 0;             // the last key started, for glide
    bool hasLast_ = false;

    double sampleRate_ = 48000.0;
    double tempo_ = 120.0;
    float noiseCoef_ = 0.0f;
    float modWheel_ = 0.0f, volume_ = 100.0f / 127.0f, expression_ = 1.0f, bend_ = 0.5f;
    float outGain_ = 0.0f;
    bool primed_ = false;
    uint64_t started_ = 0;
    uint32_t seed_ = 0x2545F491u;

    // The LFOs when they don't retrigger: one shared by every voice
    std::array<synth::Lfo, 2> global_{};
    std::array<double, 2> globalAge_{};
    std::array<float, 2> globalValue_{};
    std::array<double, 2> lfoHz_{};

    // The current control step's output before the output gain: left and right
    float acc_[2][synth::kControlFrames] = {};

    std::atomic<int> active_{0};
};

// The synthesizer as an instrument device. Its setting is its parameter values; applySetting sends
// each one to the engine.
class SynthDevice : public InstrumentDevice {
public:
    explicit SynthDevice(const DeviceSetting &setting);

    // The id of the synthesizer's device setting (presets are values, not ids)
    static constexpr const char *kId = "vibin.synth";
    // The setting of a factory preset (SynthPresets.h): every parameter at its default, then the
    // preset's values, named after it. An unknown name gives Init.
    static DeviceSetting setting(const std::string &presetName = "Init");

    Synth &synth() { return synth_; }

    DeviceKind kind() const override { return DeviceKind::Synth; }
    bool matches(const DeviceSetting &setting) const override;
    void applySetting(const DeviceSetting &setting) override;
    std::vector<DeviceParam> params() const override;
    // Its stored parameters are atomics the block takes at its start, so the audio thread may set them
    bool automatable() const override { return true; }
    void automate(uint32_t paramId, double value) override { synth_.setParam(paramId, value); }

    void prepare(double sampleRate, uint32_t maxFrames) override { synth_.prepare(sampleRate, maxFrames); }
    void process(AudioBlock &block) override { synth_.process(block); }
    void beginBlock() override { synth_.beginBlock(); }
    void noteOn(int key, float velocity) override { synth_.noteOn(key, velocity); }
    void noteOff(int key) override { synth_.noteOff(key); }
    void allNotesOff() override { synth_.allNotesOff(); }
    void controlChange(int controller, float value) override { synth_.controlChange(controller, value); }

private:
    Synth synth_;
};

}
