#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace plugin {
struct PluginConfig;
}

namespace daw {

// The user's preferences, kept in a small key=value file in their profile
// (%APPDATA%\VYBN\settings.ini on Windows) so they outlive the project.
struct Settings {
    // The sample library: imported instruments go in its Instruments folder, recordings of
    // unsaved projects in its Recordings folder, and projects default to its Projects folder
    std::string libraryPath;
    // Devices, by name (indices change as things are plugged in). An empty output is the system
    // default. The input: empty for the system default, opened only once an audio track is
    // armed; "none" for none.
    std::string audioOutput;
    std::string audioInput;
    uint32_t bufferFrames = 256;
    // MIDI inputs the user turned off; every other one plugged in is opened
    std::vector<std::string> midiDisabled;
    // The user's Freesound API key (freesound.org/apiv2/apply), for searching it in the browser
    std::string freesoundKey;
    // Performance: audio worker threads besides the audio thread (-1 for one per core but one),
    // whether long samples stream from disk, and the most memory the streamed chunks take
    int audioThreads = -1;
    bool streamSamples = true;
    uint32_t streamMemoryMB = 128;
    // Whether overlays and capture tools (Steam, OBS, Discord, vkconfig...) may hook into the
    // app's graphics (Vulkan's implicit layers). Off, they stay out: tens of MB less memory.
    bool graphicsOverlays = false;
    // How long an instrument or sound file nothing uses stays loaded (-1: until the app closes)
    int unusedSeconds = 30;
    // Windows' own open and save dialogs instead of the app's (slower to open, and unlike the app)
    bool nativeFileDialogs = false;
    // Autosave (see Autosave.h): whether a copy of an unsaved project is written every so often, how
    // long after a change (minutes, 1-30), and how many automatic versions are kept (1-50)
    bool autosave = true;
    int autosaveMinutes = 2;
    int autosaveVersions = 10;
    // Plugins: folders the user added to the ones scanned, plugins turned off (by id), whether
    // they run sandboxed, and folders each plugin may read besides its own file ("id|folder"),
    // or write in too ("id|*folder": * can't start a path)
    std::vector<std::string> pluginFolders;
    std::vector<std::string> pluginsDisabled;
    bool pluginSandbox = true;
    // Automation recording: Touch (a lane is written only while its control is held) or Latch
    bool automationTouch = false;
    // The arrangement's tempo and signature lanes shown
    bool timingLanes = true;
    // Imported and dropped files that are loops come in warped at their tempo (docs/design/warp-plan.md)
    bool warpLoops = true;
    std::vector<std::string> pluginGrants;
    // The piano roll's view (PianoRollView::writeView): its grid, scale, ghosts, lane and zoom
    std::string pianoRollView;
    // The step sequencer's view (StepSequencerView::writeView): its step size, swing, gate, velocity, hearing
    std::string stepSequencerView;
    static constexpr const char *kNoInput = "none";

    static std::string sampleCachePath(); // %LOCALAPPDATA%\VYBN\Sample cache
    static std::string pluginsPath();     // %LOCALAPPDATA%\VYBN\Plugins: imported plugins
    // The plugin folders to scan (the standard CLAP and VST3 folders, the imported plugins' and the
    // user's) and how plugins run
    plugin::PluginConfig pluginConfig() const;

    static std::string defaultLibraryPath(); // Documents\VYBN Library (or the old "Vibin Library", if it is there)
    static std::string filePath();
    // The app was called Vibin: its folders in %APPDATA% and %LOCALAPPDATA% are renamed to VYBN, once
    static void moveFromOldName();
    // Reads the file; missing keys keep their defaults
    static Settings load();
    bool save(std::string &error) const;
};

}
