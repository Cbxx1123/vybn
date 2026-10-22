#include "daw/io/SynthUserPresets.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "daw/io/Toml.h"
#include "audio/Synth.h"
#include "ui/FileDialog.h"

namespace fs = std::filesystem;

namespace daw {

namespace {

// The engine's parameter with this id, or null
const audio::DeviceParam *paramById(uint32_t id)
{
    static const std::vector<audio::DeviceParam> params = audio::synth::synthParams();
    for (const audio::DeviceParam &p : params)
        if (p.id == id)
            return &p;
    return nullptr;
}

// Turns a file's [values] table into parameter values: known ids, clamped to their ranges
plugin::ParamValues readValues(const toml::Value &root)
{
    plugin::ParamValues values;
    const toml::Value *table = root.at("values");
    if (!table || !table->isTable())
        return values;
    for (const auto &entry : table->table().entries()) {
        if (!entry.second.isNumber())
            continue;
        uint32_t id = 0;
        try {
            size_t used = 0;
            const unsigned long parsed = std::stoul(entry.first, &used);
            if (used != entry.first.size() || parsed > 0xFFFFFFFFul)
                continue;
            id = static_cast<uint32_t>(parsed);
        } catch (const std::exception &) {
            continue;
        }
        const audio::DeviceParam *p = paramById(id);
        if (!p)
            continue;
        const double value = std::clamp(entry.second.number(), p->min, p->max);
        plugin::setValue(values, id, value);
    }
    std::sort(values.begin(), values.end(), [](const auto &a, const auto &b) { return a.first < b.first; });
    return values;
}

}

std::string userSynthPresetFolder()
{
    const char *appData = std::getenv("APPDATA");
    fs::path base = appData && *appData ? fs::path(appData) : fs::path(std::getenv("HOME") ? std::getenv("HOME") : ".") / ".config";
    return (base / "VYBN" / "Synth presets").string();
}

std::vector<audio::SynthPreset> loadUserSynthPresets(std::string &error, const std::string &folder)
{
    std::vector<audio::SynthPreset> presets;
    std::error_code ec;
    if (!fs::is_directory(fs::u8path(folder), ec))
        return presets;
    std::vector<fs::path> files;
    for (const fs::directory_entry &entry : fs::directory_iterator(fs::u8path(folder), ec))
        if (entry.is_regular_file(ec) && entry.path().extension() == ".toml")
            files.push_back(entry.path());
    std::sort(files.begin(), files.end());
    std::string skipped;
    for (const fs::path &file : files) {
        std::ifstream in(file, std::ios::binary);
        std::stringstream text;
        text << in.rdbuf();
        toml::Value root;
        std::string parseError;
        const std::string name = in && toml::parse(text.str(), root, parseError) ? root.get("name", std::string()) : std::string();
        if (name.empty()) {
            skipped += (skipped.empty() ? "" : ", ") + file.filename().u8string();
            continue;
        }
        audio::SynthPreset preset;
        preset.name = name;
        preset.category = "User";
        preset.description = root.get("description", std::string());
        preset.values = readValues(root);
        presets.push_back(std::move(preset));
    }
    if (!skipped.empty())
        error = "Skipped " + skipped + ": not a synth preset";
    return presets;
}

bool saveUserSynthPreset(const audio::SynthPreset &preset, std::string &error, const std::string &folder)
{
    if (preset.name.empty()) {
        error = "A preset needs a name";
        return false;
    }
    if (ui::files::cleanFileName(preset.name) != preset.name) {
        error = "A preset's name can't contain any of  < > : \" / \\ | ? *";
        return false;
    }
    std::error_code ec;
    fs::create_directories(fs::u8path(folder), ec);
    const fs::path file = fs::u8path(folder) / (preset.name + ".toml");
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    if (!out) {
        error = "Couldn't write " + file.u8string();
        return false;
    }
    out << "# A VYBN Synth preset\n";
    out << "name = " << toml::quote(preset.name) << "\n";
    out << "category = \"User\"\n";
    if (!preset.description.empty())
        out << "description = " << toml::quote(preset.description) << "\n";
    out << "\n[values]\n";
    for (const auto &[id, value] : preset.values)
        out << toml::quote(std::to_string(id)) << " = " << toml::number(value) << "\n";
    out.close();
    if (!out) {
        error = "Couldn't write " + file.u8string();
        return false;
    }
    return true;
}

}
