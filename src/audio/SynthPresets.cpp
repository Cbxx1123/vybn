#include "SynthPresets.h"

#include "audio/Synth.h"

namespace audio {

namespace {

using namespace synth;

// Wave, filter, LFO, mode, source and target choices, by their index
constexpr double kWaveSine = 0, kWaveTriangle = 1, kWaveSaw = 2, kWaveSquare = 3, kWaveHarmonic = 5, kWaveVocal = 6,
                 kWavePwm = 7;
constexpr double kLfoSine = 0, kLfoTriangle = 1, kLfoSaw = 2;
constexpr double kFilterLp12 = 0, kFilterLp24 = 1, kFilterBand = 3;
constexpr double kModeMono = 1, kModeLegato = 2;
constexpr double kSrcLfo1 = 1, kSrcLfo2 = 2, kSrcModWheel = 7;
constexpr double kTgtPitch = 1, kTgtOsc1Shape = 4, kTgtDetune = 8, kTgtCutoff = 9;

// Osc 2, LFO 1, LFO 2 and the modulation slots, by their ids
constexpr uint32_t osc2(uint32_t offset) { return kOscBase[1] + offset; }
constexpr uint32_t lfo1(uint32_t offset) { return kLfoBase[0] + offset; }
constexpr uint32_t lfo2(uint32_t offset) { return kLfoBase[1] + offset; }
constexpr uint32_t modSource(uint32_t slot) { return kModSlotBase + 3 * slot; }
constexpr uint32_t modTarget(uint32_t slot) { return kModSlotBase + 3 * slot + 1; }
constexpr uint32_t modAmount(uint32_t slot) { return kModSlotBase + 3 * slot + 2; }

std::vector<SynthPreset> buildPresets()
{
    std::vector<SynthPreset> p;
    p.push_back({"Init", "Init", "The defaults: a saw through a low-pass filter, with a short attack and a medium release.", {}});

    // Bass
    p.push_back({"Sub Bass", "Bass", "A sine with a sub an octave down: mono, with a short glide between notes.",
                 {{kOscWave, kWaveSine}, {kOscLevel, 100}, {kOscUnison, 1}, {kOscDetune, 0}, {kSubLevel, 70},
                  {kSubOctave, 0}, {kSubWave, 0}, {kCutoff, 500}, {kResonance, 10}, {kEnvAmount, 0},
                  {kKeyTracking, 40}, {kAmpSustain, 100}, {kAmpRelease, 120}, {kVoiceMode, kModeMono},
                  {kVoices, 1}, {kGlide, 60}}});
    p.push_back({"Reese", "Bass", "Two detuned saws, each unisoned, through a low-pass filter that a slow LFO moves.",
                 {{kOscWave, kWaveSaw}, {kOscOctave, -1}, {kOscUnison, 3}, {kOscDetune, 40}, {kOscSpread, 80},
                  {osc2(kOscWave), kWaveSaw}, {osc2(kOscOctave), -1}, {osc2(kOscLevel), 80}, {osc2(kOscUnison), 3},
                  {osc2(kOscDetune), 40}, {osc2(kOscSpread), 80}, {kCutoff, 350}, {kResonance, 25}, {kEnvAmount, 10},
                  {kKeyTracking, 30}, {kVoiceMode, kModeMono}, {kVoices, 1}, {kAmpSustain, 100}, {kAmpRelease, 150},
                  {lfo1(kLfoWave), kLfoSine}, {lfo1(kLfoRate), 0.25}, {modSource(0), kSrcLfo1},
                  {modTarget(0), kTgtCutoff}, {modAmount(0), 40}}});
    p.push_back({"Acid Line", "Bass", "A squelching saw: a high-resonance low-pass with a strong, short filter envelope, played legato.",
                 {{kOscWave, kWaveSaw}, {kOscOctave, -1}, {kOscUnison, 1}, {kOscDetune, 0}, {kCutoff, 300},
                  {kResonance, 80}, {kEnvAmount, 85}, {kKeyTracking, 40}, {kFilterVelocity, 40},
                  {kFilterDecay, 180}, {kFilterSustain, 0}, {kFilterRelease, 150}, {kVoiceMode, kModeLegato},
                  {kGlide, 70}, {kVoices, 1}, {kAmpSustain, 90}, {kAmpRelease, 120}}});
    p.push_back({"Pluck Bass", "Bass", "A square and a saw, with a fast filter envelope that closes on each note.",
                 {{kOscWave, kWaveSquare}, {kOscLevel, 60}, {kOscOctave, -1}, {kOscUnison, 1},
                  {osc2(kOscWave), kWaveSaw}, {osc2(kOscLevel), 50}, {osc2(kOscOctave), -1}, {osc2(kOscUnison), 2},
                  {osc2(kOscDetune), 10}, {kCutoff, 900}, {kResonance, 15}, {kEnvAmount, 60}, {kFilterDecay, 150},
                  {kFilterSustain, 10}, {kFilterRelease, 100}, {kAmpDecay, 400}, {kAmpSustain, 0},
                  {kAmpRelease, 150}}});

    // Lead
    p.push_back({"Saw Lead", "Lead", "Five unisoned saws in mono with a glide. The mod wheel adds vibrato, and LFO 1 a little more.",
                 {{kOscUnison, 5}, {kOscDetune, 25}, {kOscSpread, 60}, {kOscLevel, 90}, {kCutoff, 6000},
                  {kResonance, 15}, {kEnvAmount, 20}, {kFilterVelocity, 30}, {kVoiceMode, kModeMono}, {kGlide, 40},
                  {kVoices, 1}, {kAmpAttack, 5}, {kAmpDecay, 200}, {kAmpSustain, 85}, {kAmpRelease, 180},
                  {lfo1(kLfoRate), 5.5}, {lfo1(kLfoFade), 600}, {modSource(0), kSrcModWheel},
                  {modTarget(0), kTgtPitch}, {modAmount(0), 8}, {modSource(1), kSrcLfo1}, {modTarget(1), kTgtPitch},
                  {modAmount(1), 2}}});
    p.push_back({"Square Lead", "Lead", "A square with a second one an octave up, in mono with a glide. LFO 1 sweeps the pulse width.",
                 {{kOscWave, kWaveSquare}, {kOscUnison, 3}, {kOscDetune, 15}, {kOscSpread, 40}, {kOscLevel, 90},
                  {osc2(kOscWave), kWaveSquare}, {osc2(kOscLevel), 40}, {osc2(kOscOctave), 1},
                  {osc2(kOscDetune), 8}, {kCutoff, 3500}, {kResonance, 30}, {kEnvAmount, 25},
                  {kVoiceMode, kModeMono}, {kGlide, 30}, {kVoices, 1}, {kAmpAttack, 3}, {kAmpDecay, 250},
                  {kAmpSustain, 80}, {kAmpRelease, 150}, {lfo1(kLfoWave), kLfoTriangle}, {lfo1(kLfoRate), 0.5},
                  {modSource(0), kSrcLfo1}, {modTarget(0), kTgtOsc1Shape}, {modAmount(0), 20}}});
    p.push_back({"Vocal Lead", "Lead", "A vocal table whose vowels move as LFO 1 shifts its shape, and the mod wheel opens the filter.",
                 {{kOscWave, kWaveVocal}, {kOscShape, 40}, {kOscLevel, 90}, {kOscUnison, 3}, {kOscDetune, 10},
                  {kOscSpread, 40}, {kCutoff, 5000}, {kResonance, 10}, {kEnvAmount, 10}, {kVoiceMode, kModeMono},
                  {kGlide, 80}, {kVoices, 1}, {kAmpAttack, 40}, {kAmpDecay, 300}, {kAmpSustain, 85},
                  {kAmpRelease, 250}, {lfo1(kLfoRate), 0.3}, {modSource(0), kSrcLfo1}, {modTarget(0), kTgtOsc1Shape},
                  {modAmount(0), 25}, {modSource(1), kSrcModWheel}, {modTarget(1), kTgtCutoff}, {modAmount(1), 20}}});
    p.push_back({"Soft Sine Lead", "Lead", "A sine with a triangle an octave up, in mono with a glide. A slow vibrato fades in.",
                 {{kOscWave, kWaveSine}, {kOscLevel, 90}, {kOscUnison, 1}, {osc2(kOscWave), kWaveTriangle},
                  {osc2(kOscLevel), 30}, {osc2(kOscOctave), 1}, {kCutoff, 2500}, {kResonance, 5},
                  {kFilterType, kFilterLp12}, {kKeyTracking, 60}, {kVoiceMode, kModeMono}, {kGlide, 60}, {kVoices, 1},
                  {kAmpAttack, 30}, {kAmpDecay, 300}, {kAmpSustain, 85}, {kAmpRelease, 400}, {lfo1(kLfoRate), 5},
                  {lfo1(kLfoFade), 500}, {modSource(0), kSrcLfo1}, {modTarget(0), kTgtPitch}, {modAmount(0), 2},
                  {modSource(1), kSrcModWheel}, {modTarget(1), kTgtCutoff}, {modAmount(1), 20}}});

    // Pad
    p.push_back({"Warm Pad", "Pad", "Detuned saws, unisoned, through a soft low-pass that a slow LFO opens and closes, with slow attack and release.",
                 {{kOscLevel, 70}, {kOscUnison, 5}, {kOscDetune, 35}, {kOscSpread, 70}, {osc2(kOscWave), kWaveSaw},
                  {osc2(kOscLevel), 60}, {osc2(kOscUnison), 5}, {osc2(kOscDetune), 30}, {osc2(kOscSpread), 70},
                  {kFilterType, kFilterLp12}, {kCutoff, 1800}, {kResonance, 10}, {kEnvAmount, 15}, {kKeyTracking, 40},
                  {kFilterAttack, 800}, {kFilterDecay, 1200}, {kFilterSustain, 60}, {kFilterRelease, 1500},
                  {kAmpAttack, 900}, {kAmpDecay, 500}, {kAmpSustain, 90}, {kAmpRelease, 1200}, {kDrift, 20},
                  {lfo1(kLfoRate), 0.15}, {modSource(0), kSrcLfo1}, {modTarget(0), kTgtCutoff}, {modAmount(0), 10}}});
    p.push_back({"Glass Pad", "Pad", "A harmonic table whose shape an LFO moves, with a long release that rings like glass.",
                 {{kOscWave, kWaveHarmonic}, {kOscShape, 30}, {kOscLevel, 80}, {kOscUnison, 3}, {kOscDetune, 10},
                  {kOscSpread, 90}, {osc2(kOscWave), kWaveTriangle}, {osc2(kOscLevel), 50}, {osc2(kOscOctave), 1},
                  {osc2(kOscUnison), 2}, {osc2(kOscDetune), 8}, {kCutoff, 6000}, {kResonance, 20}, {kEnvAmount, 10},
                  {kAmpAttack, 1200}, {kAmpDecay, 800}, {kAmpSustain, 80}, {kAmpRelease, 3000}, {kDrift, 15},
                  {lfo1(kLfoRate), 0.2}, {modSource(0), kSrcLfo1}, {modTarget(0), kTgtOsc1Shape}, {modAmount(0), 25}}});
    p.push_back({"Strings Pad", "Pad", "Two saw banks of unison, slowly bowed in and out, with an LFO that spreads their detune.",
                 {{kOscWave, kWaveSaw}, {kOscLevel, 60}, {kOscUnison, 7}, {kOscDetune, 30}, {kOscSpread, 80},
                  {osc2(kOscWave), kWaveSaw}, {osc2(kOscLevel), 50}, {osc2(kOscUnison), 5}, {osc2(kOscDetune), 25},
                  {osc2(kOscSpread), 80}, {kFilterType, kFilterLp12}, {kCutoff, 2500}, {kResonance, 0},
                  {kEnvAmount, 10}, {kKeyTracking, 50}, {kFilterAttack, 600}, {kFilterDecay, 600},
                  {kFilterSustain, 60}, {kFilterRelease, 800}, {kAmpAttack, 500}, {kAmpDecay, 400},
                  {kAmpSustain, 85}, {kAmpRelease, 800}, {kDrift, 25}, {lfo2(kLfoWave), kLfoTriangle},
                  {lfo2(kLfoRate), 0.3}, {modSource(0), kSrcLfo2}, {modTarget(0), kTgtDetune}, {modAmount(0), 20}}});
    p.push_back({"Dark Drone", "Pad", "Low saw and square with noise, under a filter that a slow LFO 2 moves. Its LFO is not synced.",
                 {{kOscWave, kWaveSaw}, {kOscLevel, 80}, {kOscOctave, -2}, {kOscUnison, 3}, {kOscDetune, 15},
                  {kOscSpread, 40}, {osc2(kOscWave), kWaveSquare}, {osc2(kOscLevel), 40}, {osc2(kOscOctave), -2},
                  {osc2(kOscDetune), 5}, {kNoiseLevel, 25}, {kNoiseColour, 80}, {kFilterType, kFilterLp24},
                  {kCutoff, 500}, {kResonance, 35}, {kEnvAmount, 0}, {kAmpAttack, 2500}, {kAmpDecay, 500},
                  {kAmpSustain, 100}, {kAmpRelease, 4000}, {kDrift, 40}, {kVoices, 6}, {lfo2(kLfoWave), kLfoSine},
                  {lfo2(kLfoRate), 0.07}, {lfo2(kLfoSync), 0}, {modSource(0), kSrcLfo2}, {modTarget(0), kTgtCutoff},
                  {modAmount(0), 35}}});

    // Keys
    p.push_back({"Electric Piano", "Keys", "A sine and a triangle tine with a fast decay; harder keys open the filter.",
                 {{kOscWave, kWaveSine}, {kOscLevel, 80}, {kOscUnison, 1}, {osc2(kOscWave), kWaveTriangle},
                  {osc2(kOscLevel), 40}, {osc2(kOscOctave), 1}, {osc2(kOscUnison), 1}, {kFilterType, kFilterLp24},
                  {kCutoff, 5000}, {kResonance, 0}, {kEnvAmount, 15}, {kKeyTracking, 60}, {kFilterVelocity, 50},
                  {kFilterDecay, 500}, {kFilterSustain, 0}, {kFilterRelease, 300}, {kAmpDecay, 700},
                  {kAmpSustain, 30}, {kAmpRelease, 400}, {kDrift, 5}}});
    p.push_back({"Organ", "Keys", "Two harmonic tables, one an octave up: an instant attack and full sustain, with no envelope shaping.",
                 {{kOscWave, kWaveHarmonic}, {kOscShape, 35}, {kOscLevel, 80}, {kOscUnison, 1}, {kOscDetune, 0},
                  {osc2(kOscWave), kWaveHarmonic}, {osc2(kOscShape), 60}, {osc2(kOscLevel), 40},
                  {osc2(kOscOctave), 1}, {osc2(kOscDetune), 0}, {osc2(kOscUnison), 1}, {kCutoff, 16000},
                  {kResonance, 0}, {kEnvAmount, 0}, {kKeyTracking, 0}, {kFilterVelocity, 0}, {kAmpAttack, 0.5},
                  {kAmpDecay, 10}, {kAmpSustain, 100}, {kAmpRelease, 60}, {kAmpVelocity, 0}, {kDrift, 0},
                  {kVoices, 8}}});
    p.push_back({"Digital Keys", "Keys", "Pulse-width tables, one an octave up, with the width moved by LFO 1 for a glassy shimmer.",
                 {{kOscWave, kWavePwm}, {kOscShape, 50}, {kOscLevel, 80}, {kOscUnison, 1},
                  {osc2(kOscWave), kWavePwm}, {osc2(kOscShape), 20}, {osc2(kOscLevel), 50}, {osc2(kOscOctave), 1},
                  {osc2(kOscDetune), 10}, {kCutoff, 7000}, {kResonance, 15}, {kEnvAmount, 25},
                  {kFilterDecay, 600}, {kFilterSustain, 40}, {kFilterRelease, 300}, {kAmpAttack, 3},
                  {kAmpDecay, 1500}, {kAmpSustain, 60}, {kAmpRelease, 350}, {lfo1(kLfoWave), kLfoTriangle},
                  {lfo1(kLfoRate), 0.5}, {modSource(0), kSrcLfo1}, {modTarget(0), kTgtOsc1Shape}, {modAmount(0), 25}}});

    // Pluck
    p.push_back({"Bell Pluck", "Pluck", "A sine with a harmonic table an octave up, struck with a long ring.",
                 {{kOscWave, kWaveSine}, {kOscOctave, 1}, {kOscLevel, 80}, {kOscUnison, 1},
                  {osc2(kOscWave), kWaveHarmonic}, {osc2(kOscShape), 50}, {osc2(kOscLevel), 35},
                  {osc2(kOscOctave), 1}, {osc2(kOscDetune), 7}, {osc2(kOscUnison), 2}, {kCutoff, 9000},
                  {kResonance, 0}, {kEnvAmount, 10}, {kFilterDecay, 300}, {kFilterSustain, 0}, {kFilterRelease, 300},
                  {kAmpAttack, 2}, {kAmpDecay, 1500}, {kAmpSustain, 0}, {kAmpRelease, 1200}, {kDrift, 0}}});
    p.push_back({"Synth Pluck", "Pluck", "A saw and a square that the filter snaps shut on each note.",
                 {{kOscWave, kWaveSaw}, {kOscUnison, 1}, {kOscLevel, 80}, {osc2(kOscWave), kWaveSquare},
                  {osc2(kOscLevel), 50}, {osc2(kOscDetune), 5}, {osc2(kOscUnison), 1}, {kCutoff, 1400},
                  {kResonance, 25}, {kEnvAmount, 70}, {kKeyTracking, 40}, {kFilterVelocity, 40}, {kFilterAttack, 2},
                  {kFilterDecay, 250}, {kFilterSustain, 0}, {kFilterRelease, 200}, {kAmpAttack, 2}, {kAmpDecay, 500},
                  {kAmpSustain, 0}, {kAmpRelease, 200}}});
    p.push_back({"Mallet", "Pluck", "A harmonic table and a soft triangle with a little noise at the strike, damped by a short decay.",
                 {{kOscWave, kWaveHarmonic}, {kOscShape, 25}, {kOscLevel, 80}, {kOscUnison, 1}, {kOscDetune, 0},
                  {osc2(kOscWave), kWaveTriangle}, {osc2(kOscLevel), 30}, {osc2(kOscOctave), 1},
                  {osc2(kOscDetune), 4}, {osc2(kOscUnison), 1}, {kNoiseLevel, 10}, {kNoiseColour, 30},
                  {kFilterType, kFilterLp12}, {kCutoff, 3000}, {kResonance, 0}, {kEnvAmount, 25},
                  {kFilterDecay, 250}, {kFilterSustain, 0}, {kFilterRelease, 250}, {kAmpAttack, 2},
                  {kAmpDecay, 900}, {kAmpSustain, 0}, {kAmpRelease, 500}}});

    // Brass & strings
    p.push_back({"Synth Brass", "Brass & strings", "Two saws in unison whose filter swells open with each note.",
                 {{kOscWave, kWaveSaw}, {kOscLevel, 80}, {kOscUnison, 3}, {kOscDetune, 15}, {kOscSpread, 40},
                  {osc2(kOscWave), kWaveSaw}, {osc2(kOscLevel), 70}, {osc2(kOscDetune), 10}, {osc2(kOscUnison), 3},
                  {osc2(kOscSpread), 40}, {kCutoff, 900}, {kResonance, 15}, {kEnvAmount, 55}, {kKeyTracking, 40},
                  {kFilterVelocity, 40}, {kFilterAttack, 120}, {kFilterDecay, 400}, {kFilterSustain, 70},
                  {kFilterRelease, 250}, {kAmpAttack, 80}, {kAmpDecay, 300}, {kAmpSustain, 85}, {kAmpRelease, 250},
                  {kAmpVelocity, 80}}});
    p.push_back({"String Ensemble", "Brass & strings", "Two saw banks of seven, bowed in slowly, with an LFO that stirs their detune.",
                 {{kOscWave, kWaveSaw}, {kOscLevel, 70}, {kOscUnison, 7}, {kOscDetune, 25}, {kOscSpread, 90},
                  {osc2(kOscWave), kWaveSaw}, {osc2(kOscLevel), 60}, {osc2(kOscUnison), 7}, {osc2(kOscDetune), 18},
                  {osc2(kOscSpread), 90}, {kFilterType, kFilterLp12}, {kCutoff, 4000}, {kResonance, 0},
                  {kEnvAmount, 0}, {kKeyTracking, 50}, {kAmpAttack, 800}, {kAmpDecay, 500}, {kAmpSustain, 90},
                  {kAmpRelease, 1000}, {kDrift, 20}, {lfo2(kLfoWave), kLfoTriangle}, {lfo2(kLfoRate), 0.35},
                  {modSource(0), kSrcLfo2}, {modTarget(0), kTgtDetune}, {modAmount(0), 15}}});

    // FX
    p.push_back({"Riser", "FX", "Noise and a slow saw LFO that climbs in pitch and opens the filter over two seconds.",
                 {{kOscWave, kWaveSaw}, {kOscLevel, 60}, {kOscUnison, 3}, {kOscDetune, 30}, {kNoiseLevel, 60},
                  {kNoiseColour, 20}, {kFilterType, kFilterLp12}, {kCutoff, 800}, {kResonance, 20}, {kEnvAmount, 0},
                  {kAmpAttack, 1500}, {kAmpDecay, 500}, {kAmpSustain, 100}, {kAmpRelease, 2000}, {kVoices, 4},
                  {lfo1(kLfoWave), kLfoSaw}, {lfo1(kLfoRate), 0.25}, {modSource(0), kSrcLfo1}, {modTarget(0), kTgtPitch},
                  {modAmount(0), 15}, {modSource(1), kSrcLfo1}, {modTarget(1), kTgtCutoff}, {modAmount(1), 60}}});
    p.push_back({"Noise Sweep", "FX", "Band-passed noise that a slow triangle LFO sweeps up and down.",
                 {{kOscLevel, 0}, {kNoiseLevel, 100}, {kNoiseColour, 40}, {kFilterType, kFilterBand}, {kCutoff, 1200},
                  {kResonance, 60}, {kDrive, 20}, {kEnvAmount, 0}, {kAmpAttack, 300}, {kAmpDecay, 1000},
                  {kAmpSustain, 60}, {kAmpRelease, 800}, {kVoices, 4}, {lfo1(kLfoWave), kLfoTriangle},
                  {lfo1(kLfoRate), 0.15}, {modSource(0), kSrcLfo1}, {modTarget(0), kTgtCutoff}, {modAmount(0), 80}}});
    p.push_back({"Wobble", "FX", "A saw and a square through a high-resonance filter that LFO 1 wobbles in eighth notes, in time with the tempo.",
                 {{kOscWave, kWaveSaw}, {kOscLevel, 80}, {kOscUnison, 3}, {kOscDetune, 15}, {osc2(kOscWave), kWaveSquare},
                  {osc2(kOscLevel), 40}, {osc2(kOscDetune), 10}, {kCutoff, 900}, {kResonance, 85}, {kEnvAmount, 0},
                  {kAmpAttack, 2}, {kAmpDecay, 300}, {kAmpSustain, 80}, {kAmpRelease, 200}, {kVoices, 4},
                  {lfo1(kLfoWave), kLfoTriangle}, {lfo1(kLfoSync), 1}, {lfo1(kLfoSyncRate), 5},
                  {modSource(0), kSrcLfo1}, {modTarget(0), kTgtCutoff}, {modAmount(0), 50}}});

    return p;
}

}

const std::vector<SynthPreset> &synthPresets()
{
    static const std::vector<SynthPreset> kPresets = buildPresets();
    return kPresets;
}

const SynthPreset *findSynthPreset(const std::string &name)
{
    for (const SynthPreset &preset : synthPresets())
        if (preset.name == name)
            return &preset;
    return nullptr;
}

}
