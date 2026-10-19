#pragma once

#include <string>
#include <vector>

#include "audio/SynthPresets.h"

namespace daw {

// The folder the user's synth presets are kept in: %APPDATA%\Vibin\Synth presets
std::string userSynthPresetFolder();

// The user's presets in `folder`, one TOML file each, sorted by file name. Each comes back with the
// category "User"; values are clamped to their parameters' ranges and unknown ids are dropped. A
// file that can't be read is skipped, and `error` names it. A folder that doesn't exist is empty.
std::vector<audio::SynthPreset> loadUserSynthPresets(std::string &error, const std::string &folder = userSynthPresetFolder());

// Writes a preset as <folder>\<name>.toml (making the folder). A name must be a file name: not
// empty, and without the characters Windows refuses (< > : " / \ | ? * or control characters); then
// `error` says why and nothing is written. Returns whether it was saved.
bool saveUserSynthPreset(const audio::SynthPreset &preset, std::string &error, const std::string &folder = userSynthPresetFolder());

}
