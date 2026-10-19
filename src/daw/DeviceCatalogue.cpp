#include "DeviceCatalogue.h"

#include <algorithm>
#include <cctype>

#include "audio/Effects.h"
#include "audio/PluginInstrument.h"
#include "audio/Synth.h"
#include "audio/SynthPresets.h"

namespace daw {

namespace {

std::string lower(std::string s)
{
    for (char &c : s)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

const char *builtinCategory(audio::EffectType type)
{
    using T = audio::EffectType;
    switch (type) {
    case T::Compressor:
    case T::Limiter:
    case T::Gate: return "Dynamics";
    case T::Equalizer:
    case T::Filter: return "EQ & filter";
    case T::Delay:
    case T::Reverb: return "Delay & reverb";
    case T::Saturation: return "Distortion";
    case T::Gain: return "Utility";
    }
    return "Other";
}

const char *builtinDescription(audio::EffectType type)
{
    using T = audio::EffectType;
    switch (type) {
    case T::Gain: return "Level and phase.";
    case T::Equalizer: return "Low shelf, a mid peak and a high shelf.";
    case T::Filter: return "Low-, high- or band-pass.";
    case T::Gate: return "Silences the sound below a threshold.";
    case T::Compressor: return "Evens out the level: threshold, ratio, attack and release.";
    case T::Limiter: return "Keeps peaks under a ceiling, looking ahead 1.5 ms.";
    case T::Saturation: return "Soft, hard or folding drive.";
    case T::Delay: return "Echoes with feedback, tone and ping-pong.";
    case T::Reverb: return "A room: size, damping and width.";
    }
    return "";
}

}

const std::vector<std::string> &DeviceCatalogue::effectCategories()
{
    static const std::vector<std::string> kCategories = {"Dynamics", "EQ & filter", "Delay & reverb", "Distortion", "Modulation",
                                                         "Utility", "Other"};
    return kCategories;
}

std::string DeviceCatalogue::pluginCategory(const plugin::Descriptor &d)
{
    if (d.instrument()) {
        if (d.hasFeature("drum-machine") || d.hasFeature("drum"))
            return "Drums";
        // By its name first ("Grand Piano" is Keys); its description can mislead ("sine voices")
        std::string category = guessCategory(d.name);
        if (category == "Other" || category.empty())
            category = d.hasFeature("sampler") ? "Keys" : "Synths";
        return category;
    }
    if (d.hasFeature("compressor") || d.hasFeature("limiter") || d.hasFeature("gate") || d.hasFeature("expander") ||
        d.hasFeature("transient-shaper"))
        return "Dynamics";
    if (d.hasFeature("equalizer") || d.hasFeature("filter"))
        return "EQ & filter";
    if (d.hasFeature("delay") || d.hasFeature("reverb"))
        return "Delay & reverb";
    if (d.hasFeature("distortion"))
        return "Distortion";
    if (d.hasFeature("chorus") || d.hasFeature("flanger") || d.hasFeature("phaser") || d.hasFeature("tremolo") ||
        d.hasFeature("pitch-shifter") || d.hasFeature("frequency-shifter"))
        return "Modulation";
    if (d.hasFeature("utility") || d.hasFeature("analyzer") || d.hasFeature("mixing") || d.hasFeature("mastering"))
        return "Utility";
    return "Other";
}

DeviceCatalogue::DeviceCatalogue(const InstrumentLibrary &library, plugin::PluginManager *plugins)
    : library_(library), plugins_(plugins)
{
}

void DeviceCatalogue::refresh()
{
    const uint64_t lib = library_.version();
    const uint64_t plug = plugins_ ? plugins_->version() : 0;
    std::vector<std::string> disabled = plugins_ ? plugins_->config().disabled : std::vector<std::string>{};
    if (lib == libraryVersion_ && plug == pluginsVersion_ && disabled == disabled_)
        return;
    libraryVersion_ = lib;
    pluginsVersion_ = plug;
    disabled_ = std::move(disabled);
    ++version_;
    entries_.clear();

    std::vector<CatalogueEntry> instruments, effects;
    {
        CatalogueEntry e;
        e.device = audio::SynthDevice::setting();
        e.device.name = "Vibin Synth";
        e.instrument = true;
        e.category = "Synths";
        e.source = "Built-in";
        e.description = "A subtractive and wavetable synthesizer: two oscillators with unison, sub and noise, a filter with its own envelope, two LFOs and a modulation matrix.";
        instruments.push_back(std::move(e));
    }
    // The synthesizer's factory presets, one entry each (Init is the entry above)
    for (const audio::SynthPreset &preset : audio::synthPresets()) {
        if (preset.name == "Init")
            continue;
        CatalogueEntry e;
        e.device = audio::SynthDevice::setting(preset.name);
        e.preset = preset.name;
        e.instrument = true;
        e.category = preset.category == "Bass" || preset.category == "Keys" ? preset.category : "Synths";
        e.source = "Vibin Synth";
        e.description = preset.description;
        instruments.push_back(std::move(e));
    }
    for (const LibraryInstrument &i : library_.entries()) {
        CatalogueEntry e;
        e.device = audio::SamplerDevice::setting(i.name);
        e.instrument = true;
        e.category = i.category;
        e.source = "Library";
        e.description = "Samples from your library, played by the sampler.";
        e.path = i.folder;
        instruments.push_back(std::move(e));
    }
    for (int t = 0; t < audio::kEffectTypeCount; ++t) {
        const auto type = static_cast<audio::EffectType>(t);
        CatalogueEntry e;
        e.device = audio::effectSetting(type);
        e.category = builtinCategory(type);
        e.source = "Built-in";
        e.description = builtinDescription(type);
        effects.push_back(std::move(e));
    }
    if (plugins_) {
        for (const plugin::Descriptor &d : plugins_->plugins()) {
            if (!plugins_->enabled(d.id))
                continue;
            CatalogueEntry e;
            e.device.kind = audio::DeviceKind::Plugin;
            e.device.id = d.id;
            e.device.name = d.name.empty() ? d.id : d.name;
            e.instrument = d.instrument();
            e.category = pluginCategory(d);
            e.source = d.vendor.empty() ? "Plugin" : d.vendor;
            e.description = d.description;
            e.path = d.path;
            (e.instrument ? instruments : effects).push_back(std::move(e));
        }
    }
    auto byName = [](const CatalogueEntry &a, const CatalogueEntry &b) { return lower(a.device.name) < lower(b.device.name); };
    std::stable_sort(instruments.begin(), instruments.end(), byName);
    std::stable_sort(effects.begin(), effects.end(), byName);
    entries_ = std::move(instruments);
    entries_.insert(entries_.end(), effects.begin(), effects.end());
}

const std::vector<CatalogueEntry> &DeviceCatalogue::entries()
{
    refresh();
    return entries_;
}

uint64_t DeviceCatalogue::version()
{
    refresh();
    return version_;
}

const CatalogueEntry *DeviceCatalogue::find(const std::string &key)
{
    refresh();
    for (const CatalogueEntry &e : entries_)
        if (e.key() == key)
            return &e;
    return nullptr;
}

std::string DeviceCatalogue::keyOf(const audio::DeviceSetting &setting)
{
    if (setting.kind == audio::DeviceKind::Synth && setting.name != "Init" && audio::findSynthPreset(setting.name))
        return setting.key() + "#" + setting.name;
    return setting.key();
}

std::vector<std::string> DeviceCatalogue::categories(bool instruments)
{
    refresh();
    std::vector<std::string> out;
    const std::vector<std::string> &order = instruments ? defaultCategories() : effectCategories();
    for (const std::string &c : order) {
        for (const CatalogueEntry &e : entries_) {
            if (e.instrument == instruments && e.category == c) {
                out.push_back(c);
                break;
            }
        }
    }
    // Categories of the user's own, after the usual ones
    for (const CatalogueEntry &e : entries_)
        if (e.instrument == instruments && std::find(out.begin(), out.end(), e.category) == out.end())
            out.push_back(e.category);
    return out;
}

}
