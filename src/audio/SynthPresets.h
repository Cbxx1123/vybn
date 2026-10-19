#pragma once

#include <string>
#include <vector>

#include "audio/Device.h"

namespace audio {

// A factory preset of the built-in synthesizer: its name, the category the catalogue files it in
// ("Bass", "Lead", "Pad", "Keys", "Pluck", "Brass & strings", "FX", "Init"), a sentence about it, and
// the parameter values that differ from the defaults (applied on top of them).
struct SynthPreset {
    std::string name;
    std::string category;
    std::string description;
    plugin::ParamValues values;
};

// Every factory preset, the Init preset (the defaults) first. Names are unique.
const std::vector<SynthPreset> &synthPresets();
// The preset with this name, or nullptr
const SynthPreset *findSynthPreset(const std::string &name);

}
