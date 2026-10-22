#include "daw/app/Settings.h"

#include "plugin/Manager.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>

namespace daw {

namespace fs = std::filesystem;

namespace {

std::string environment(const char *name)
{
    const char *value = std::getenv(name);
    return value ? value : "";
}

std::string trim(std::string s)
{
    const auto first = s.find_first_not_of(" \t\r\n");
    const auto last = s.find_last_not_of(" \t\r\n");
    return first == std::string::npos ? std::string() : s.substr(first, last - first + 1);
}

// Moves what is in `from` into `to`: an entry `to` lacks is moved whole, a folder both have is merged
// the same way, and a file both have stays as `to` has it (written since, so never the older), the
// copy in `from` removed. `from` goes once it is empty.
void mergeInto(const fs::path &from, const fs::path &to)
{
    std::error_code ec;
    fs::create_directories(to, ec);
    std::vector<fs::path> entries; // listed first: the folder changes as they move
    for (const fs::directory_entry &entry : fs::directory_iterator(from, ec))
        entries.push_back(entry.path());
    for (const fs::path &entry : entries) {
        const fs::path target = to / entry.filename();
        std::error_code e;
        if (!fs::exists(target, e))
            fs::rename(entry, target, e);
        else if (fs::is_directory(entry, e) && fs::is_directory(target, e))
            mergeInto(entry, target);
        else if (!fs::is_directory(entry, e))
            fs::remove(entry, e);
    }
    if (fs::is_empty(from, ec))
        fs::remove(from, ec);
}

}

std::string Settings::defaultLibraryPath()
{
    std::string home = environment("USERPROFILE");
    if (home.empty())
        home = environment("HOME");
    // The library from before the app was renamed, where there is one and no new one yet: it stays
    // where it is, since instruments may be referred to by path
    const fs::path library = fs::path(home) / "Documents" / "VYBN Library";
    const fs::path before = fs::path(home) / "Documents" / "Vibin Library";
    std::error_code ec;
    if (!fs::exists(library, ec) && fs::is_directory(before, ec))
        return before.string();
    return library.string();
}

void Settings::moveFromOldName()
{
    // Settings, keybinds, the theme and synth presets (roaming), and the sample cache, imported
    // plugins and autosaves (local): moved from the "Vibin" folders into the "VYBN" ones, merged with
    // anything already there. What can't be moved (in use) stays to be tried at the next start.
    for (const char *variable : {"APPDATA", "LOCALAPPDATA"}) {
        const std::string base = environment(variable);
        if (base.empty())
            continue;
        const fs::path from = fs::path(base) / "Vibin";
        std::error_code ec;
        if (fs::is_directory(from, ec))
            mergeInto(from, fs::path(base) / "VYBN");
    }
}

std::string Settings::filePath()
{
    std::string base = environment("APPDATA");
    if (base.empty())
        base = (fs::path(environment("HOME")) / ".config").string();
    return (fs::path(base) / "VYBN" / "settings.ini").string();
}

std::string Settings::sampleCachePath()
{
    std::string base = environment("LOCALAPPDATA");
    if (base.empty())
        base = (fs::path(environment("HOME")) / ".cache").string();
    return (fs::path(base) / "VYBN" / "Sample cache").string();
}

std::string Settings::pluginsPath()
{
    std::string base = environment("LOCALAPPDATA");
    if (base.empty())
        base = (fs::path(environment("HOME")) / ".local").string();
    return (fs::path(base) / "VYBN" / "Plugins").string();
}

plugin::PluginConfig Settings::pluginConfig() const
{
    plugin::PluginConfig config;
    config.folders = plugin::PluginManager::standardFolders();
    config.folders.push_back(pluginsPath());
    for (const std::string &folder : pluginFolders)
        config.folders.push_back(folder);
    config.sandbox = pluginSandbox;
    config.cacheFile = (fs::path(filePath()).parent_path() / "plugins.cache").string();
    config.disabled = pluginsDisabled;
    for (const std::string &grant : pluginGrants) {
        const size_t bar = grant.find('|');
        const std::string folder = grant.substr(bar + 1);
        if (!folder.empty() && folder[0] == '*')
            config.writeGrants[grant.substr(0, bar)].push_back(folder.substr(1));
        else
            config.grants[grant.substr(0, bar)].push_back(folder);
    }
    return config;
}

Settings Settings::load()
{
    Settings settings;
    settings.libraryPath = defaultLibraryPath();
    std::ifstream file(filePath());
    std::string line;
    while (std::getline(file, line)) {
        const auto equals = line.find('=');
        if (line.empty() || line[0] == '#' || equals == std::string::npos)
            continue;
        const std::string key = trim(line.substr(0, equals));
        const std::string value = trim(line.substr(equals + 1));
        if (key == "library" && !value.empty())
            settings.libraryPath = value;
        else if (key == "audioOutput")
            settings.audioOutput = value;
        else if (key == "audioInput")
            settings.audioInput = value;
        else if (key == "bufferFrames" && !value.empty())
            settings.bufferFrames = static_cast<uint32_t>(std::clamp(std::atoi(value.c_str()), 32, 4096));
        else if (key == "freesoundKey")
            settings.freesoundKey = value;
        else if (key == "audioThreads" && !value.empty())
            settings.audioThreads = std::clamp(std::atoi(value.c_str()), -1, 63);
        else if (key == "streamSamples")
            settings.streamSamples = value != "0";
        else if (key == "graphicsOverlays")
            settings.graphicsOverlays = value != "0";
        else if (key == "nativeFileDialogs")
            settings.nativeFileDialogs = value != "0";
        else if (key == "autosave")
            settings.autosave = value != "0";
        else if (key == "autosaveMinutes" && !value.empty())
            settings.autosaveMinutes = std::clamp(std::atoi(value.c_str()), 1, 30);
        else if (key == "autosaveVersions" && !value.empty())
            settings.autosaveVersions = std::clamp(std::atoi(value.c_str()), 1, 50);
        else if (key == "automationTouch")
            settings.automationTouch = value != "0";
        else if (key == "pianoRoll")
            settings.pianoRollView = value;
        else if (key == "stepSequencer")
            settings.stepSequencerView = value;
        else if (key == "unusedSeconds" && !value.empty())
            settings.unusedSeconds = std::clamp(std::atoi(value.c_str()), -1, 86400);
        else if (key == "streamMemoryMB" && !value.empty())
            settings.streamMemoryMB = static_cast<uint32_t>(std::clamp(std::atoi(value.c_str()), 16, 8192));
        else if (key == "midiDisabled" && !value.empty())
            settings.midiDisabled.push_back(value);
        else if (key == "pluginFolder" && !value.empty())
            settings.pluginFolders.push_back(value);
        else if (key == "pluginDisabled" && !value.empty())
            settings.pluginsDisabled.push_back(value);
        else if (key == "pluginGrant" && value.find('|') != std::string::npos)
            settings.pluginGrants.push_back(value);
        else if (key == "pluginSandbox")
            settings.pluginSandbox = value != "0";
    }
    return settings;
}

bool Settings::save(std::string &error) const
{
    const fs::path path = filePath();
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    std::ofstream file(path, std::ios::trunc);
    if (!file) {
        error = "Can't write " + path.string();
        return false;
    }
    file << "# VYBN settings\n";
    file << "library=" << libraryPath << "\n";
    file << "audioOutput=" << audioOutput << "\n";
    file << "audioInput=" << audioInput << "\n";
    file << "bufferFrames=" << bufferFrames << "\n";
    file << "freesoundKey=" << freesoundKey << "\n";
    file << "audioThreads=" << audioThreads << "\n";
    file << "streamSamples=" << (streamSamples ? 1 : 0) << "\n";
    file << "graphicsOverlays=" << (graphicsOverlays ? 1 : 0) << "\n";
    file << "nativeFileDialogs=" << (nativeFileDialogs ? 1 : 0) << "\n";
    file << "autosave=" << (autosave ? 1 : 0) << "\n";
    file << "autosaveMinutes=" << autosaveMinutes << "\n";
    file << "autosaveVersions=" << autosaveVersions << "\n";
    file << "automationTouch=" << (automationTouch ? 1 : 0) << "\n";
    if (!pianoRollView.empty())
        file << "pianoRoll=" << pianoRollView << "\n";
    if (!stepSequencerView.empty())
        file << "stepSequencer=" << stepSequencerView << "\n";
    file << "unusedSeconds=" << unusedSeconds << "\n";
    file << "streamMemoryMB=" << streamMemoryMB << "\n";
    for (const std::string &name : midiDisabled)
        file << "midiDisabled=" << name << "\n";
    file << "pluginSandbox=" << (pluginSandbox ? 1 : 0) << "\n";
    for (const std::string &folder : pluginFolders)
        file << "pluginFolder=" << folder << "\n";
    for (const std::string &id : pluginsDisabled)
        file << "pluginDisabled=" << id << "\n";
    for (const std::string &grant : pluginGrants)
        file << "pluginGrant=" << grant << "\n";
    return static_cast<bool>(file);
}

}
