#include "Synth.h"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <utility>

#include "SynthPresets.h"

namespace audio {

namespace synth {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kTau = 2.0 * kPi;
constexpr double kCentre = 0.70710678118654752440; // -3 dB: a sound panned to the middle in both channels
constexpr double kSixtyDb = 6.90775527898;         // ln(1000): an exponential that falls 60 dB in its time
constexpr float kDone = 1e-4f;                     // an envelope this quiet has finished
// Beats in one cycle of each sync rate: 4 bars, 2 bars, 1 bar, 1/2, 1/4, 1/8, 1/16, 1/32, then the
// triplets (x 2/3) and the dotted notes (x 1.5)
constexpr double kSyncBeats[13] = {16.0, 8.0, 4.0, 2.0, 1.0, 0.5, 0.25, 0.125,
                                   2.0 / 3.0, 1.0 / 3.0, 1.0 / 6.0, 1.5, 0.75};

// The sums of the modulation matrix for one voice in one control step
struct ModSums {
    float pitch = 0.0f;
    float oscPitch[2] = {0.0f, 0.0f};
    float shape[2] = {0.0f, 0.0f};
    float level[2] = {0.0f, 0.0f};
    float detune = 0.0f;
    float cutoff = 0.0f; // octaves
    float resonance = 0.0f;
    float drive = 0.0f;
    float noise = 0.0f;
    float amp = 1.0f;    // a multiplier
    float pan = 0.0f;
    float lfoOct[2] = {0.0f, 0.0f};
};

DeviceParam &add(std::vector<DeviceParam> &list, uint32_t id, const std::string &module, const std::string &name,
                 double min, double max, double def, ParamUnit unit)
{
    DeviceParam p;
    p.id = id;
    p.module = module;
    p.name = name;
    p.min = min;
    p.max = max;
    p.def = def;
    p.unit = unit;
    list.push_back(p);
    return list.back();
}

void choice(std::vector<DeviceParam> &list, uint32_t id, const std::string &module, const std::string &name, int def,
            const std::vector<std::string> &names)
{
    DeviceParam &p = add(list, id, module, name, 0.0, static_cast<double>(names.size() - 1), def, ParamUnit::Choice);
    p.stepped = true;
    p.choices = names;
}

void envelope(std::vector<DeviceParam> &list, uint32_t base, const std::string &module, double attack, double decay,
              double sustain, double release)
{
    add(list, base, module, "Attack", 0.5, 10000.0, attack, ParamUnit::Milliseconds).logarithmic = true;
    add(list, base + 1, module, "Decay", 1.0, 10000.0, decay, ParamUnit::Milliseconds).logarithmic = true;
    add(list, base + 2, module, "Sustain", 0.0, 100.0, sustain, ParamUnit::Percent);
    add(list, base + 3, module, "Release", 1.0, 15000.0, release, ParamUnit::Milliseconds).logarithmic = true;
}

std::vector<DeviceParam> buildParams()
{
    std::vector<DeviceParam> list;
    const std::vector<std::string> waves = {"Sine",        "Triangle",       "Saw",         "Square",
                                            "Basic table", "Harmonic table", "Vocal table", "PWM table"};
    for (int o = 0; o < 2; ++o) {
        const uint32_t b = kOscBase[o];
        const std::string module = o == 0 ? "Osc 1" : "Osc 2";
        choice(list, b + kOscWave, module, "Wave", o == 0 ? 2 : 3, waves);
        add(list, b + kOscShape, module, "Shape", 0.0, 100.0, 50.0, ParamUnit::Percent);
        add(list, b + kOscLevel, module, "Level", 0.0, 100.0, o == 0 ? 80.0 : 0.0, ParamUnit::Percent);
        add(list, b + kOscOctave, module, "Octave", -3.0, 3.0, 0.0, ParamUnit::None).stepped = true;
        add(list, b + kOscSemitones, module, "Semitones", -12.0, 12.0, 0.0, ParamUnit::Semitones).stepped = true;
        add(list, b + kOscFine, module, "Fine", -100.0, 100.0, 0.0, ParamUnit::None);
        add(list, b + kOscUnison, module, "Unison", 1.0, 7.0, 1.0, ParamUnit::None).stepped = true;
        add(list, b + kOscDetune, module, "Detune", 0.0, 100.0, 20.0, ParamUnit::Percent);
        add(list, b + kOscSpread, module, "Spread", 0.0, 100.0, 50.0, ParamUnit::Percent);
    }

    add(list, kSubLevel, "Sub & noise", "Sub level", 0.0, 100.0, 0.0, ParamUnit::Percent);
    choice(list, kSubOctave, "Sub & noise", "Sub octave", 0, {"-1 octave", "-2 octaves"});
    choice(list, kSubWave, "Sub & noise", "Sub wave", 0, {"Sine", "Square"});
    add(list, kNoiseLevel, "Sub & noise", "Noise level", 0.0, 100.0, 0.0, ParamUnit::Percent);
    add(list, kNoiseColour, "Sub & noise", "Noise colour", 0.0, 100.0, 50.0, ParamUnit::Percent);

    choice(list, kFilterType, "Filter", "Type", 1,
           {"Low-pass 12", "Low-pass 24", "High-pass 12", "Band-pass", "Notch"});
    add(list, kCutoff, "Filter", "Cutoff", 20.0, 20000.0, 4000.0, ParamUnit::Hertz).logarithmic = true;
    add(list, kResonance, "Filter", "Resonance", 0.0, 100.0, 20.0, ParamUnit::Percent);
    add(list, kDrive, "Filter", "Drive", 0.0, 100.0, 0.0, ParamUnit::Percent);
    add(list, kEnvAmount, "Filter", "Env amount", -100.0, 100.0, 30.0, ParamUnit::Percent);
    add(list, kKeyTracking, "Filter", "Key tracking", 0.0, 100.0, 50.0, ParamUnit::Percent);
    add(list, kFilterVelocity, "Filter", "Velocity", 0.0, 100.0, 20.0, ParamUnit::Percent);

    envelope(list, kAmpAttack, "Amp envelope", 2.0, 300.0, 80.0, 250.0);
    add(list, kAmpVelocity, "Amp envelope", "Velocity", 0.0, 100.0, 70.0, ParamUnit::Percent);
    envelope(list, kFilterAttack, "Filter envelope", 2.0, 400.0, 30.0, 300.0);

    const std::vector<std::string> lfoWaves = {"Sine", "Triangle", "Saw", "Square", "Random", "Smooth random"};
    const std::vector<std::string> syncRates = {"4 bars", "2 bars", "1 bar", "1/2",  "1/4",  "1/8",  "1/16",
                                                "1/32",   "1/4T",   "1/8T",  "1/16T", "1/4.", "1/8."};
    for (int k = 0; k < 2; ++k) {
        const uint32_t b = kLfoBase[k];
        const std::string module = k == 0 ? "LFO 1" : "LFO 2";
        choice(list, b + kLfoWave, module, "Wave", 0, lfoWaves);
        add(list, b + kLfoRate, module, "Rate", 0.02, 40.0, k == 0 ? 4.0 : 0.5, ParamUnit::Hertz).logarithmic = true;
        add(list, b + kLfoSync, module, "Sync", 0.0, 1.0, 0.0, ParamUnit::Toggle).stepped = true;
        choice(list, b + kLfoSyncRate, module, "Sync rate", 4, syncRates);
        add(list, b + kLfoRetrigger, module, "Retrigger", 0.0, 1.0, 1.0, ParamUnit::Toggle).stepped = true;
        add(list, b + kLfoFade, module, "Fade in", 0.0, 5000.0, 0.0, ParamUnit::Milliseconds);
    }

    const std::vector<std::string> sources = {"None",          "LFO 1",   "LFO 2", "Filter envelope", "Amp envelope",
                                              "Velocity",      "Key",     "Mod wheel", "Random"};
    const std::vector<std::string> targets = {"None",      "Pitch",       "Osc 1 pitch", "Osc 2 pitch",
                                              "Osc 1 shape", "Osc 2 shape", "Osc 1 level", "Osc 2 level",
                                              "Detune",    "Cutoff",      "Resonance",   "Drive",
                                              "Noise level", "Amp",       "Pan",         "LFO 1 rate",
                                              "LFO 2 rate"};
    for (uint32_t slot = 0; slot < 6; ++slot) {
        const uint32_t b = kModSlotBase + 3 * slot;
        const std::string n = std::to_string(slot + 1);
        choice(list, b, "Modulation", "Slot " + n + " source", 0, sources);
        choice(list, b + 1, "Modulation", "Slot " + n + " target", 0, targets);
        add(list, b + 2, "Modulation", "Slot " + n + " amount", -100.0, 100.0, 0.0, ParamUnit::Percent);
    }

    choice(list, kVoiceMode, "Voice", "Mode", 0, {"Poly", "Mono", "Legato"});
    add(list, kVoices, "Voice", "Voices", 1.0, 16.0, 8.0, ParamUnit::None).stepped = true;
    add(list, kGlide, "Voice", "Glide", 0.0, 2000.0, 0.0, ParamUnit::Milliseconds);
    add(list, kBendRange, "Voice", "Bend range", 0.0, 24.0, 2.0, ParamUnit::Semitones).stepped = true;
    add(list, kDrift, "Voice", "Drift", 0.0, 100.0, 10.0, ParamUnit::Percent);
    add(list, kVoiceLevel, "Voice", "Level", -36.0, 6.0, -6.0, ParamUnit::Decibels);
    return list;
}

uint32_t nextRandom(uint32_t &s)
{
    s ^= s << 13;
    s ^= s >> 17;
    s ^= s << 5;
    return s;
}

// Random numbers in [0, 1) and in [-1, 1)
double unitRandom(uint32_t &s)
{
    return nextRandom(s) * (1.0 / 4294967296.0);
}

double bipolarRandom(uint32_t &s)
{
    return unitRandom(s) * 2.0 - 1.0;
}

// The change that brings an envelope to its target within 60 dB over `seconds`, per `dt`
double toward(double dt, double seconds)
{
    return 1.0 - std::exp(-kSixtyDb * dt / std::max(seconds, 1e-4));
}

// One envelope step: attack linear to 1, decay and release exponential (60 dB in their time)
void stepEnvelope(Envelope &e, double dt, double attack, double decay, double sustain, double release)
{
    switch (e.stage) {
    case Envelope::Stage::Idle:
        e.level = 0.0f;
        break;
    case Envelope::Stage::Attack:
        e.level += static_cast<float>(dt / std::max(attack, 1e-4));
        if (e.level >= 1.0f) {
            e.level = 1.0f;
            e.stage = Envelope::Stage::Decay;
        }
        break;
    case Envelope::Stage::Decay:
        e.level += static_cast<float>((sustain - e.level) * toward(dt, decay));
        if (std::fabs(e.level - sustain) < kDone) {
            e.level = static_cast<float>(sustain);
            e.stage = Envelope::Stage::Sustain;
        }
        break;
    case Envelope::Stage::Sustain:
        e.level = static_cast<float>(sustain);
        break;
    case Envelope::Stage::Release:
        e.level -= static_cast<float>(e.level * toward(dt, release));
        if (e.level < kDone) {
            e.level = 0.0f;
            e.stage = Envelope::Stage::Idle;
        }
        break;
    }
}

// An LFO advanced by dt seconds at `hz`, its value (-1..1) at its phase. Random values are drawn
// once per cycle.
float advanceLfo(Lfo &l, int wave, double hz, double dt, uint32_t &seed)
{
    l.phase += hz * dt;
    while (l.phase >= 1.0) {
        l.phase -= 1.0;
        l.heldPrev = l.held;
        l.held = static_cast<float>(bipolarRandom(seed));
    }
    const double p = l.phase;
    switch (wave) {
    case 0: return static_cast<float>(std::sin(kTau * p));
    case 1: return static_cast<float>(4.0 * std::fabs(p - 0.5) - 1.0);
    case 2: return static_cast<float>(2.0 * p - 1.0);
    case 3: return p < 0.5 ? 1.0f : -1.0f;
    case 4: return l.held;
    default: return l.heldPrev + (l.held - l.heldPrev) * static_cast<float>(p);
    }
}

// How far an LFO has faded in: 0 at the note on, 1 after its fade time
float fadeDepth(double age, float fade)
{
    if (fade <= 0.0f)
        return 1.0f;
    return static_cast<float>(std::min(1.0, age / fade));
}

// A table read at a phase (0-1): linear between samples. The table repeats its first sample at
// the end, so the last sample interpolates without a wrap.
inline float readCycle(const float *t, double phase)
{
    const double x = phase * kTableSize;
    const int i = std::min(static_cast<int>(x), kTableSize - 1);
    const float f = static_cast<float>(x - i);
    return t[i] + (t[i + 1] - t[i]) * f;
}

// One oscillator sample at a phase (0-1)
inline float readOsc(const OscRead &o, double phase)
{
    switch (o.mode) {
    case 0: {
        // The phase skew: the wave's peak moves to the skew point
        const double s = o.skew;
        const double skewed = phase < s ? 0.5 * phase / s : 0.5 + 0.5 * (phase - s) / (1.0 - s);
        return readCycle(o.a, skewed);
    }
    case 1: {
        // A pulse: the saw less the saw a width later (its DC is already zero)
        double w = phase + o.width;
        if (w >= 1.0)
            w -= 1.0;
        return readCycle(o.a, phase) - readCycle(o.a, w);
    }
    default: {
        const float v = readCycle(o.a, phase);
        return o.b ? v + (readCycle(o.b, phase) - v) * o.frac : v;
    }
    }
}

// The read of an oscillator for its wave and shape, at the mip level its fastest copy needs
OscRead oscRead(const WaveBank &bank, int wave, double shape, double maxInc)
{
    OscRead r;
    const int mip = WaveBank::levelFor(maxInc);
    if (wave <= kSaw) {
        r.mode = 0;
        r.a = bank.level(wave, 0, mip);
        r.skew = 0.05 + 0.9 * shape;
    } else if (wave == kSquare) {
        r.mode = 1;
        r.a = bank.level(kSaw, 0, mip);
        r.width = 0.05 + 0.9 * shape;
    } else {
        // The tables: the shape is the position over the frames
        r.mode = 2;
        const int frames = bank.frames(wave);
        const double pos = shape * (frames - 1);
        const int i0 = std::min(static_cast<int>(pos), frames - 1);
        const int i1 = std::min(i0 + 1, frames - 1);
        r.frac = static_cast<float>(pos - i0);
        r.a = bank.level(wave, i0, mip);
        r.b = i1 != i0 ? bank.level(wave, i1, mip) : nullptr;
    }
    return r;
}

// One step of a state-variable filter (Zavalishin's TPT form): its low-pass, band-pass and high-pass
struct Svf {
    double low;
    double band;
    double high;
};

inline Svf svfStep(double x, double a1, double a2, double a3, double k, double &ic1, double &ic2)
{
    const double v3 = x - ic2;
    const double v1 = a1 * ic1 + a2 * v3;
    const double v2 = ic2 + a2 * ic1 + a3 * v3;
    ic1 = 2.0 * v1 - ic1;
    ic2 = 2.0 * v2 - ic2;
    return {v2, v1, x - k * v1 - v2};
}

}

// The parameters in the order the device shows them: the most useful first, then by id
std::vector<DeviceParam> synthParams()
{
    const std::vector<DeviceParam> all = buildParams();
    const uint32_t first[] = {kCutoff, kResonance, kEnvAmount, kAmpAttack, kAmpRelease,
                              kOscBase[0] + kOscDetune, kGlide, kVoiceLevel};
    std::vector<DeviceParam> out;
    for (uint32_t id : first)
        for (const DeviceParam &p : all)
            if (p.id == id)
                out.push_back(p);
    for (const DeviceParam &p : all)
        if (std::find(std::begin(first), std::end(first), p.id) == std::end(first))
            out.push_back(p);
    return out;
}

}

namespace synth {

void previewCycle(int wave, float shape, float *out, int count)
{
    if (!out || count <= 0)
        return;
    // Level 0 (the top harmonic at full band), one frame at the shape: the engine's own read
    const OscRead read = oscRead(WaveBank::get(), std::clamp(wave, 0, kWaveCount - 1), std::clamp(static_cast<double>(shape), 0.0, 1.0), 0.0);
    for (int i = 0; i < count; ++i)
        out[i] = readOsc(read, static_cast<double>(i) / count);
}

}

Synth::Synth(std::string name)
    : Instrument(std::move(name))
    , bank_(&synth::WaveBank::get())
{
    for (const DeviceParam &p : synth::synthParams()) {
        const uint32_t id = p.id;
        known_[id] = true;
        low_[id] = static_cast<float>(p.min);
        high_[id] = static_cast<float>(p.max);
        // Percent-style rows are read as 0-1 and millisecond rows in seconds
        scale_[id] = p.unit == ParamUnit::Percent ? 0.01f : p.unit == ParamUnit::Milliseconds ? 0.001f : 1.0f;
        stored_[id].store(static_cast<float>(p.def), std::memory_order_relaxed);
    }
    beginBlock();
}

void Synth::setParam(uint32_t id, double value)
{
    if (id >= synth::kSynthParamSlots || !known_[id] || !std::isfinite(value))
        return;
    stored_[id].store(std::clamp(static_cast<float>(value), low_[id], high_[id]), std::memory_order_relaxed);
}

double Synth::param(uint32_t id) const
{
    if (id >= synth::kSynthParamSlots || !known_[id])
        return 0.0;
    return stored_[id].load(std::memory_order_relaxed);
}

void Synth::prepare(double sampleRate, uint32_t)
{
    sampleRate_ = sampleRate;
    noiseCoef_ = static_cast<float>(1.0 - std::exp(-synth::kTau * 600.0 / sampleRate));
    for (synth::Voice &v : voices_)
        v = synth::Voice();
    heldCount_ = 0;
    hasLast_ = false;
    global_ = {};
    globalAge_ = {};
    globalValue_ = {};
    lfoHz_ = {};
    active_.store(0, std::memory_order_relaxed);
    primed_ = false;
}

void Synth::beginBlock()
{
    for (uint32_t id = 0; id < synth::kSynthParamSlots; ++id)
        if (known_[id])
            p_[id] = stored_[id].load(std::memory_order_relaxed) * scale_[id];
}

int Synth::mode() const
{
    return std::clamp(static_cast<int>(std::lround(p_[synth::kVoiceMode])), 0, 2);
}

int Synth::voiceLimit() const
{
    return std::clamp(static_cast<int>(std::lround(p_[synth::kVoices])), 1, synth::kMaxVoices);
}

int Synth::unison(int osc) const
{
    return std::clamp(static_cast<int>(std::lround(p_[synth::kOscBase[osc] + synth::kOscUnison])), 1,
                      synth::kMaxUnison);
}

int Synth::lfoWave(int lfo) const
{
    return std::clamp(static_cast<int>(std::lround(p_[synth::kLfoBase[lfo] + synth::kLfoWave])), 0, 5);
}

// An LFO's rate in Hz: its own, or with Sync the transport's tempo in its beats
double Synth::baseRate(int lfo) const
{
    const uint32_t b = synth::kLfoBase[lfo];
    if (p_[b + synth::kLfoSync] > 0.5f) {
        const int index = std::clamp(static_cast<int>(std::lround(p_[b + synth::kLfoSyncRate])), 0, 12);
        return (tempo_ / 60.0) / synth::kSyncBeats[index];
    }
    return p_[b + synth::kLfoRate];
}

void Synth::process(AudioBlock &block)
{
    beginBlock();
    tempo_ = block.transport.tempo;
    // The output level: the voice level and the controllers' volume and expression (100/127 is unity)
    const float volume = volume_ / (100.0f / 127.0f);
    const float target = fromDecibels(p_[synth::kVoiceLevel]) * volume * volume * expression_ * expression_;
    if (!primed_) {
        outGain_ = target;
        primed_ = true;
    }
    const bool stereo = block.channelCount > 1;
    float *left = block.channels[0];
    float *right = stereo ? block.channels[1] : left;
    for (uint32_t done = 0; done < block.frames;) {
        const uint32_t n = std::min(synth::kControlFrames, block.frames - done);
        controlStep(n);
        // The output gain glides across the step, so changes don't click
        const float g0 = outGain_;
        const float step = (target - g0) / static_cast<float>(n);
        for (uint32_t i = 0; i < n; ++i) {
            const float g = g0 + step * static_cast<float>(i);
            if (stereo) {
                left[done + i] += acc_[0][i] * g;
                right[done + i] += acc_[1][i] * g;
            } else {
                left[done + i] += 0.5f * (acc_[0][i] + acc_[1][i]) * g;
            }
        }
        outGain_ = target;
        done += n;
    }
    int count = 0;
    for (const synth::Voice &v : voices_)
        if (v.amp.stage != synth::Envelope::Stage::Idle)
            ++count;
    active_.store(count, std::memory_order_relaxed);
}

void Synth::controlStep(uint32_t n)
{
    const double dt = n / sampleRate_;
    for (float *row : acc_)
        for (uint32_t i = 0; i < n; ++i)
            row[i] = 0.0f;
    // The shared LFOs (those that don't retrigger) advance once for every voice
    for (int k = 0; k < 2; ++k) {
        lfoHz_[k] = baseRate(k);
        if (!retrigger(k)) {
            const float depth = synth::fadeDepth(globalAge_[k], p_[synth::kLfoBase[k] + synth::kLfoFade]);
            globalValue_[k] = synth::advanceLfo(global_[k], lfoWave(k), lfoHz_[k], dt, seed_) * depth;
            globalAge_[k] += dt;
        }
    }
    for (synth::Voice &v : voices_) {
        if (v.amp.stage == synth::Envelope::Stage::Idle)
            continue;
        updateVoice(v, dt, n);
        renderVoice(v, n);
    }
}

void Synth::updateVoice(synth::Voice &v, double dt, uint32_t n)
{
    synth::stepEnvelope(v.amp, dt, p_[synth::kAmpAttack], p_[synth::kAmpDecay], p_[synth::kAmpSustain],
                        p_[synth::kAmpRelease]);
    synth::stepEnvelope(v.filt, dt, p_[synth::kFilterAttack], p_[synth::kFilterDecay], p_[synth::kFilterSustain],
                        p_[synth::kFilterRelease]);

    // The LFOs: a voice's own when they retrigger, else the shared ones
    float lfoValue[2];
    for (int k = 0; k < 2; ++k) {
        if (retrigger(k)) {
            v.lfoAge[k] += dt;
            lfoValue[k] = synth::advanceLfo(v.lfo[k], lfoWave(k), lfoHz_[k] * std::exp2(v.lfoOct[k]), dt, v.seed) *
                          synth::fadeDepth(v.lfoAge[k], p_[synth::kLfoBase[k] + synth::kLfoFade]);
        } else {
            lfoValue[k] = globalValue_[k];
        }
    }

    // The glide: the pitch slides from the note it started from, exponentially
    const double tau = p_[synth::kGlide] / 3.0;
    v.glide = tau > 1e-6 ? v.glide * std::exp(-dt / tau) : 0.0;

    // The modulation matrix: each slot adds its source times its amount to its target
    const float source[9] = {0.0f,
                             lfoValue[0],
                             lfoValue[1],
                             v.filt.level,
                             v.amp.level,
                             v.velocity,
                             static_cast<float>((v.key - 60) / 48.0),
                             modWheel_,
                             v.random};
    synth::ModSums m;
    for (uint32_t slot = 0; slot < 6; ++slot) {
        const uint32_t b = synth::kModSlotBase + 3 * slot;
        const int src = std::clamp(static_cast<int>(std::lround(p_[b])), 0, 8);
        const int tgt = std::clamp(static_cast<int>(std::lround(p_[b + 1])), 0, 16);
        const float amount = p_[b + 2];
        if (src == 0 || tgt == 0 || amount == 0.0f)
            continue;
        const float x = source[src] * amount;
        switch (tgt) {
        case 1: m.pitch += x * 24.0f; break;
        case 2: m.oscPitch[0] += x * 24.0f; break;
        case 3: m.oscPitch[1] += x * 24.0f; break;
        case 4: m.shape[0] += x; break;
        case 5: m.shape[1] += x; break;
        case 6: m.level[0] += x; break;
        case 7: m.level[1] += x; break;
        case 8: m.detune += x; break;
        case 9: m.cutoff += x * 6.0f; break;
        case 10: m.resonance += x; break;
        case 11: m.drive += x; break;
        case 12: m.noise += x; break;
        case 13: m.amp *= std::max(0.0f, 1.0f + x); break;
        case 14: m.pan += x; break;
        case 15: m.lfoOct[0] += x * 4.0f; break;
        default: m.lfoOct[1] += x * 4.0f; break;
        }
    }
    // Rates from the matrix apply from the next step on
    v.lfoOct[0] = m.lfoOct[0];
    v.lfoOct[1] = m.lfoOct[1];

    // The pitch of each oscillator in semitones: the key, the bend, the glide, the matrix and the
    // octave, semitone and fine settings
    const double bend = (bend_ - 0.5) * 2.0 * p_[synth::kBendRange];
    const double base = v.key + bend + v.glide + m.pitch;
    double semis[2];
    for (int o = 0; o < 2; ++o) {
        const uint32_t b = synth::kOscBase[o];
        semis[o] = base + p_[b + synth::kOscOctave] * 12.0 + p_[b + synth::kOscSemitones] +
                   p_[b + synth::kOscFine] * 0.01 + m.oscPitch[o];
    }

    for (int o = 0; o < 2; ++o) {
        const uint32_t b = synth::kOscBase[o];
        const int count = unison(o);
        const int wave = std::clamp(static_cast<int>(std::lround(p_[b + synth::kOscWave])), 0, 7);
        const double detune = std::clamp(static_cast<double>(p_[b + synth::kOscDetune]) + m.detune, 0.0, 1.0);
        const double spread = std::clamp(static_cast<double>(p_[b + synth::kOscSpread]), 0.0, 1.0);
        const float level = std::clamp(p_[b + synth::kOscLevel] + m.level[o], 0.0f, 1.0f);
        const double shape = std::clamp(static_cast<double>(p_[b + synth::kOscShape]) + m.shape[o], 0.0, 1.0);
        const double driftCents = static_cast<double>(p_[synth::kDrift]) * 8.0;
        const double hz = 440.0 * std::pow(2.0, (semis[o] - 69.0) / 12.0);
        const double base440 = hz / sampleRate_;
        const double unisonGain = 1.0 / std::sqrt(static_cast<double>(count));
        v.count[o] = count;
        v.sounding[o] = level > 0.0f ? count : 0;
        for (int i = 0; i < count; ++i) {
            synth::Copy &c = v.copy[o][i];
            // The copies spread evenly across the unison: their detune and their pan
            c.pos = count == 1 ? 0.0 : 2.0 * i / (count - 1) - 1.0;
            c.driftTime -= dt;
            if (c.driftTime <= 0.0) {
                c.driftTarget = synth::bipolarRandom(v.seed) * driftCents;
                c.driftTime = 0.5;
            }
            c.drift += (c.driftTarget - c.drift) * (1.0 - std::exp(-dt / 0.1));
            const double cents = c.pos * detune * 50.0 + c.drift;
            const double inc = base440 * std::pow(2.0, cents / 1200.0);
            if (v.fresh) {
                c.inc = inc;
                c.incStep = 0.0;
            } else {
                c.incStep = (inc - c.inc) / static_cast<double>(n);
            }
            const double pan = std::clamp(c.pos * spread + m.pan, -1.0, 1.0);
            const double theta = (pan + 1.0) * synth::kPi / 4.0;
            const double gain = level * unisonGain;
            c.gainL = static_cast<float>(gain * std::cos(theta));
            c.gainR = static_cast<float>(gain * std::sin(theta));
        }
        v.read[o] = synth::oscRead(*bank_, wave, shape, base440 * std::pow(2.0, (50.0 * detune + driftCents) / 1200.0));
    }

    // The sub: the first oscillator's pitch an octave or two down, a sine or a square
    const double subSemis = semis[0] - (static_cast<int>(std::lround(p_[synth::kSubOctave])) == 0 ? 12.0 : 24.0);
    v.subInc = 440.0 * std::pow(2.0, (subSemis - 69.0) / 12.0) / sampleRate_;
    v.subGain = p_[synth::kSubLevel] * static_cast<float>(synth::kCentre);
    const int subWave = static_cast<int>(std::lround(p_[synth::kSubWave])) == 0 ? synth::kSine : synth::kSquare;
    v.subRead = synth::oscRead(*bank_, subWave, 0.5, v.subInc);

    v.noiseGain = std::clamp(p_[synth::kNoiseLevel] + m.noise, 0.0f, 1.0f) * 0.5f;
    v.noiseColour = std::clamp(p_[synth::kNoiseColour], 0.0f, 1.0f);

    // The filter: cutoff from its setting and the key, the envelope, the velocity and the matrix
    const double octaves = p_[synth::kKeyTracking] * ((v.key - 60) / 12.0) +
                           p_[synth::kEnvAmount] * 6.0 * v.filt.level +
                           p_[synth::kFilterVelocity] * 2.0 * v.velocity + m.cutoff;
    const double sr = sampleRate_;
    const double cutoff = std::clamp(p_[synth::kCutoff] * std::exp2(octaves), 20.0, 0.45 * sr);
    v.filterType = std::clamp(static_cast<int>(std::lround(p_[synth::kFilterType])), 0, 4);
    float resonance = std::clamp(p_[synth::kResonance] + m.resonance, 0.0f, 1.0f);
    if (v.filterType == 1)
        resonance *= 0.7f; // the two poles of the 24 dB filter share the resonance
    v.k = 2.0 - 1.96 * resonance;
    const double g = std::tan(synth::kPi * cutoff / sr);
    v.a1 = 1.0 / (1.0 + g * (g + v.k));
    v.a2 = g * v.a1;
    v.a3 = g * v.a2;
    v.drive = std::clamp(p_[synth::kDrive] + m.drive, 0.0f, 1.0f);

    // The amp: the envelope, the velocity's share of the level and the matrix's multiplier, gliding
    // across the step
    const float vs = p_[synth::kAmpVelocity];
    const float velocityLevel = 1.0f - vs + vs * v.velocity * v.velocity;
    v.ampTarget = v.amp.level * velocityLevel * std::max(0.0f, m.amp);
    v.ampStep = (v.ampTarget - v.ampGain) / static_cast<float>(n);
    v.fresh = false;
}

float Synth::runFilter(synth::Voice &v, int ch, float x)
{
    double in = x;
    if (v.drive > 0.0f) {
        const double d = v.drive;
        in = std::tanh(in * (1.0 + 4.0 * d)) / (1.0 + 1.5 * d);
    }
    const synth::Svf s = synth::svfStep(in, v.a1, v.a2, v.a3, v.k, v.ic1[ch][0], v.ic2[ch][0]);
    switch (v.filterType) {
    case 0: return static_cast<float>(s.low);
    case 1: return static_cast<float>(synth::svfStep(s.low, v.a1, v.a2, v.a3, v.k, v.ic1[ch][1], v.ic2[ch][1]).low);
    case 2: return static_cast<float>(s.high);
    case 3: return static_cast<float>(s.band);
    default: return static_cast<float>(s.low + s.high);
    }
}

void Synth::renderVoice(synth::Voice &v, uint32_t n)
{
    float *accL = acc_[0];
    float *accR = acc_[1];
    for (uint32_t i = 0; i < n; ++i) {
        float l = 0.0f, r = 0.0f;
        for (int o = 0; o < 2; ++o) {
            for (int c = 0; c < v.sounding[o]; ++c) {
                synth::Copy &cp = v.copy[o][c];
                const float s = synth::readOsc(v.read[o], cp.phase);
                l += s * cp.gainL;
                r += s * cp.gainR;
                cp.phase += cp.inc;
                if (cp.phase >= 1.0)
                    cp.phase -= 1.0;
                cp.inc += cp.incStep;
            }
        }
        if (v.subGain > 0.0f) {
            const float s = synth::readOsc(v.subRead, v.subPhase) * v.subGain;
            l += s;
            r += s;
            v.subPhase += v.subInc;
            if (v.subPhase >= 1.0)
                v.subPhase -= 1.0;
        }
        if (v.noiseGain > 0.0f) {
            for (int ch = 0; ch < 2; ++ch) {
                const float white = static_cast<float>(synth::unitRandom(v.noise)) * 2.0f - 1.0f;
                v.noiseLp[ch] += noiseCoef_ * (white - v.noiseLp[ch]);
                const float mixed = (1.0f - v.noiseColour) * white + v.noiseColour * v.noiseLp[ch];
                if (ch == 0)
                    l += mixed * v.noiseGain;
                else
                    r += mixed * v.noiseGain;
            }
        }
        const float g = v.ampGain;
        v.ampGain += v.ampStep;
        accL[i] += runFilter(v, 0, l) * g;
        accR[i] += runFilter(v, 1, r) * g;
    }
    v.ampGain = v.ampTarget;
}

synth::Voice &Synth::pickVoice(int limit)
{
    // A free voice first
    for (int i = 0; i < limit; ++i)
        if (voices_[i].amp.stage == synth::Envelope::Stage::Idle)
            return voices_[i];
    // Else the released voice that is quietest
    synth::Voice *best = nullptr;
    for (int i = 0; i < limit; ++i) {
        synth::Voice &v = voices_[i];
        if (v.amp.stage == synth::Envelope::Stage::Release && (!best || v.amp.level < best->amp.level))
            best = &v;
    }
    if (best)
        return *best;
    // Else the oldest
    best = &voices_[0];
    for (int i = 1; i < limit; ++i)
        if (voices_[i].started < best->started)
            best = &voices_[i];
    return *best;
}

void Synth::startVoice(synth::Voice &v, int key, float velocity, double glideSemis)
{
    using Stage = synth::Envelope::Stage;
    if (v.amp.stage == Stage::Idle) {
        // A voice that was silent starts from silence, its filters clear
        v.ampGain = 0.0f;
        for (int ch = 0; ch < 2; ++ch)
            for (int s = 0; s < 2; ++s) {
                v.ic1[ch][s] = 0.0;
                v.ic2[ch][s] = 0.0;
            }
    }
    v.key = key;
    v.velocity = velocity;
    v.started = ++started_;
    v.glide = p_[synth::kGlide] > 0.0f ? glideSemis : 0.0;
    v.seed = synth::nextRandom(seed_) | 1u;
    v.noise = synth::nextRandom(seed_) | 1u;
    v.random = static_cast<float>(synth::bipolarRandom(seed_));
    v.amp.stage = Stage::Attack;
    v.filt.stage = Stage::Attack;
    v.fresh = true;
    v.subPhase = 0.0;
    // Each note's oscillators start at random phases, except a single copy, which starts at 0
    for (int o = 0; o < 2; ++o) {
        const int count = unison(o);
        for (int i = 0; i < synth::kMaxUnison; ++i) {
            synth::Copy &c = v.copy[o][i];
            c.phase = (count == 1 && i == 0) ? 0.0 : synth::unitRandom(seed_);
            c.drift = 0.0;
            c.driftTarget = 0.0;
            c.driftTime = 0.0;
        }
    }
    for (int k = 0; k < 2; ++k) {
        v.lfo[k] = synth::Lfo();
        v.lfo[k].held = static_cast<float>(synth::bipolarRandom(seed_));
        v.lfo[k].heldPrev = v.lfo[k].held;
        v.lfoAge[k] = 0.0;
        v.lfoOct[k] = 0.0;
    }
}

void Synth::releaseVoice(synth::Voice &v)
{
    using Stage = synth::Envelope::Stage;
    if (v.amp.stage != Stage::Idle && v.amp.stage != Stage::Release)
        v.amp.stage = Stage::Release;
    if (v.filt.stage != Stage::Idle && v.filt.stage != Stage::Release)
        v.filt.stage = Stage::Release;
}

void Synth::pushHeld(int key, float velocity)
{
    removeHeld(key);
    if (heldCount_ == static_cast<int>(held_.size())) {
        // Full: the oldest key goes
        for (std::size_t i = 1; i < held_.size(); ++i)
            held_[i - 1] = held_[i];
        --heldCount_;
    }
    held_[static_cast<std::size_t>(heldCount_++)] = Held{key, velocity};
}

bool Synth::removeHeld(int key)
{
    for (int i = 0; i < heldCount_; ++i) {
        if (held_[static_cast<std::size_t>(i)].key != key)
            continue;
        for (int j = i + 1; j < heldCount_; ++j)
            held_[static_cast<std::size_t>(j - 1)] = held_[static_cast<std::size_t>(j)];
        --heldCount_;
        return true;
    }
    return false;
}

void Synth::noteOn(int key, float velocity)
{
    velocity = std::clamp(velocity, 0.0f, 1.0f);
    // A glide starts from the last key played, in semitones
    const double offset = (hasLast_ && p_[synth::kGlide] > 0.0f) ? static_cast<double>(lastKey_ - key) : 0.0;
    const int voicing = mode();
    if (voicing == 0) {
        // Poly: a key that is already sounding keeps its voice; else a free voice, else one is stolen
        bool anySounding = false;
        for (const synth::Voice &v : voices_)
            anySounding = anySounding || v.amp.stage != synth::Envelope::Stage::Idle;
        if (!anySounding) {
            globalAge_ = {0.0, 0.0};
        }
        const int limit = voiceLimit();
        synth::Voice *v = nullptr;
        for (int i = 0; i < limit && !v; ++i)
            if (voices_[i].amp.stage != synth::Envelope::Stage::Idle && voices_[i].key == key)
                v = &voices_[i];
        if (!v)
            v = &pickVoice(limit);
        startVoice(*v, key, velocity, offset);
    } else {
        // Mono and Legato: one voice, and the keys held in order. Legato, with a note still
        // sounding, only changes the pitch: the envelopes carry on
        pushHeld(key, velocity);
        synth::Voice &v = voices_[0];
        const bool sounding = v.amp.stage == synth::Envelope::Stage::Attack ||
                              v.amp.stage == synth::Envelope::Stage::Decay ||
                              v.amp.stage == synth::Envelope::Stage::Sustain;
        if (voicing == 2 && sounding) {
            v.glide = p_[synth::kGlide] > 0.0f ? offset : 0.0;
            v.key = key;
        } else {
            startVoice(v, key, velocity, offset);
        }
    }
    lastKey_ = key;
    hasLast_ = true;
}

void Synth::noteOff(int key)
{
    if (mode() == 0) {
        for (synth::Voice &v : voices_)
            if (v.amp.stage != synth::Envelope::Stage::Idle && v.key == key)
                releaseVoice(v);
        return;
    }
    removeHeld(key);
    synth::Voice &v = voices_[0];
    if (heldCount_ == 0) {
        releaseVoice(v);
        return;
    }
    // Back to the key still held: the pitch slides there from the key released
    if (v.key == key && v.amp.stage != synth::Envelope::Stage::Idle) {
        const Held &top = held_[static_cast<std::size_t>(heldCount_ - 1)];
        v.key = top.key;
        v.velocity = top.velocity;
        v.glide = p_[synth::kGlide] > 0.0f ? static_cast<double>(key - top.key) : 0.0;
    }
}

void Synth::allNotesOff()
{
    for (synth::Voice &v : voices_)
        releaseVoice(v);
    heldCount_ = 0;
}

void Synth::controlChange(int controller, float value)
{
    if (!std::isfinite(value))
        return;
    value = std::clamp(value, 0.0f, 1.0f);
    switch (controller) {
    case 1: modWheel_ = value; break;
    case 7: volume_ = value; break;
    case 11: expression_ = value; break;
    case kPitchBend: bend_ = value; break;
    case 121: // reset all controllers
        modWheel_ = 0.0f;
        volume_ = 100.0f / 127.0f;
        expression_ = 1.0f;
        bend_ = 0.5f;
        break;
    case 123: allNotesOff(); break;
    default: break;
    }
}

SynthDevice::SynthDevice(const DeviceSetting &setting)
    : InstrumentDevice("Vibin Synth")
{
    applySetting(setting);
}

bool SynthDevice::matches(const DeviceSetting &setting) const
{
    return setting.kind == kind() && setting.id == kId;
}

void SynthDevice::applySetting(const DeviceSetting &setting)
{
    for (const DeviceParam &p : synth::synthParams())
        synth_.setParam(p.id, setting.value(p.id, p.def));
}

std::vector<DeviceParam> SynthDevice::params() const
{
    return synth::synthParams();
}

DeviceSetting SynthDevice::setting(const std::string &presetName)
{
    DeviceSetting s;
    s.kind = DeviceKind::Synth;
    s.id = kId;
    s.name = "Init";
    for (const DeviceParam &p : synth::synthParams())
        s.setValue(p.id, p.def);
    // A preset's values on top of the defaults, named after it; an unknown name is Init
    if (const SynthPreset *preset = findSynthPreset(presetName)) {
        for (const auto &v : preset->values)
            s.setValue(v.first, v.second);
        s.name = preset->name;
    }
    return s;
}

}
