#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "daw/library/Library.h"
#include "audio/Device.h"
#include "plugin/Manager.h"

namespace daw {

// One device that can be put in a chain, as the pickers and the browser show it
struct CatalogueEntry {
    audio::DeviceSetting device; // its kind, id and name, at its defaults
    std::string preset;          // a synthesizer preset's name (its own entry), else empty
    bool instrument = false;     // plays notes (else an effect)
    std::string category;        // "Guitars", "Synths", "Dynamics", "Delay & reverb"...
    std::string source;          // "Library", "Built-in", or a plugin's vendor
    std::string description;
    std::string path;            // a library instrument's folder, a plugin's .clap
    bool plugin() const { return device.kind == audio::DeviceKind::Plugin; }
    // The device's key, with the preset's name after a '#' when there is one ("synth:vibin.synth#Reese")
    std::string key() const { return preset.empty() ? device.key() : device.key() + "#" + preset; }
};

// Everything that can be put in a chain, in one list: the library's instruments (on the sampler),
// the built-in effects, and the plugins installed (instruments and effects; those turned off in
// Settings are left out). Rebuilt when the library or the plugins change. Main thread.
class DeviceCatalogue {
public:
    DeviceCatalogue(const InstrumentLibrary &library, plugin::PluginManager *plugins);

    const std::vector<CatalogueEntry> &entries();
    // Goes up whenever the list changes
    uint64_t version();
    // The entry with this key (CatalogueEntry::key), or nullptr
    const CatalogueEntry *find(const std::string &key);
    // The catalogue key of a device in a project: its setting's key, with the preset's name when it
    // is a synthesizer preset's (so the picker can mark the preset in use)
    static std::string keyOf(const audio::DeviceSetting &setting);
    // The categories instruments or effects are in, in their usual order, each once
    std::vector<std::string> categories(bool instruments);
    // A new device for an entry, at its defaults
    audio::DeviceSetting make(const CatalogueEntry &entry) const { return entry.device; }

    // Where a plugin belongs: an instrument among the library's categories (by its features and
    // name), an effect among the built-in effects' categories (by its features)
    static std::string pluginCategory(const plugin::Descriptor &d);
    static const std::vector<std::string> &effectCategories();

private:
    void refresh();

    const InstrumentLibrary &library_;
    plugin::PluginManager *plugins_;
    std::vector<CatalogueEntry> entries_;
    uint64_t libraryVersion_ = ~0ull;
    uint64_t pluginsVersion_ = ~0ull;
    uint64_t version_ = 0;
    std::vector<std::string> disabled_; // the plugins turned off when last built
};

}
