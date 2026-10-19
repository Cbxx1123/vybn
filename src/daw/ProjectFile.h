#pragma once

#include <string>
#include <vector>

#include "Project.h"

namespace daw {

// Project files (.vbp) are TOML. The root holds the format version, the project's name, tempo,
// time signature and length, and the list of library instruments its tracks play; then one
// [[tracks]] table per track (name, kind, gain, pan, mute, solo, armed, and for audio tracks
// input_channel, input_stereo, monitor), each with its devices, a [tracks.instrument] table
// (instrument tracks) and [[tracks.effects]] tables, and [[tracks.clips]] tables (name, start,
// length; for instrument tracks an array of inline note tables: start, length, key, velocity; for
// audio tracks audio_file, offset, gain, fade_in, fade_out, in seconds). [[master_effects]] tables
// hold the master's devices. A device's table has its kind ("sampler", "effect" or "plugin"), id,
// name, bypass, a plugin's state (base64) and params, an array of {id, value} tables.
// [[export_presets]] tables (name, and [[export_presets.targets]] with every ExportTarget field,
// enums by name) and an optional [export_metadata] table hold the export settings; a file with no
// export_presets key gets the defaults, and "export_presets = []" means none.
// Positions are in beats. Sound files inside the project's folder are written relative to it,
// others in full; relative paths read against the folder.
//
// Format 3 made instruments and effects devices. Older formats are refused.
//
// Reading ignores keys it doesn't know, so newer files with extra fields still open, but refuses
// other format versions. Missing values take their defaults; values of the wrong type, unknown
// track kinds and devices, and broken TOML are errors. Values out of range are clamped. The
// selection isn't saved.

constexpr const char *kProjectExtension = ".vbp";
constexpr int kProjectFormat = 3;

// `folder` is the project file's folder (empty: write and read paths as they are)
std::string writeProject(const Project &project, const std::string &folder = {});
bool readProject(const std::string &text, Project &project, std::string &error, const std::string &folder = {});

// The library instruments the tracks play, each once, in track order
std::vector<std::string> instrumentsUsed(const Project &project);

// Files. Saving writes a temporary file beside the target and then replaces it, so a failed save
// leaves the old file whole.
bool saveProjectFile(const std::string &path, const Project &project, std::string &error);
bool loadProjectFile(const std::string &path, Project &project, std::string &error);

}
