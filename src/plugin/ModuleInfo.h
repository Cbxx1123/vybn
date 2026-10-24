#pragma once

// What a VST3 plugin is, without loading it: the subcategories of its classes as the feature words the
// app asks (shared with the plugin host, which reads them from the plugin's factory), and the
// `moduleinfo.json` a bundle may carry, which lists its classes so that it needn't be loaded in a host
// to be scanned.

#include <algorithm>
#include <cctype>
#include <map>
#include <string>
#include <vector>

#include "plugin/Protocol.h"

namespace plugin {

// A class's subcategories ("Instrument|Synth", "Fx|Delay") as CLAP's feature words ("instrument
// synthesizer", "audio-effect delay"); one that is neither an instrument nor an effect is an effect
inline std::string vst3Features(const std::string &subcategories)
{
    static const std::map<std::string, std::string> words = {
        {"instrument", "instrument"}, {"fx", "audio-effect"},      {"synth", "synthesizer"},   {"sampler", "sampler"},
        {"drum", "drum-machine"},     {"delay", "delay"},          {"reverb", "reverb"},       {"dynamics", "compressor"},
        {"eq", "equalizer"},          {"filter", "filter"},        {"distortion", "distortion"}, {"analyzer", "analyzer"},
        {"modulation", "modulation"}, {"pitch shift", "pitch-shifter"}, {"mastering", "mastering"}, {"restoration", "restoration"},
        {"mono", "mono"},             {"stereo", "stereo"},        {"surround", "surround"},   {"tools", "utility"},
        {"generator", "instrument"},  {"piano", "piano"},          {"external", "external"},   {"spatial", "spatial"},
    };
    std::string out;
    bool instrument = false, effect = false;
    size_t start = 0;
    while (start <= subcategories.size()) {
        size_t end = subcategories.find('|', start);
        if (end == std::string::npos)
            end = subcategories.size();
        std::string word = subcategories.substr(start, end - start);
        std::transform(word.begin(), word.end(), word.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        start = end + 1;
        if (word.empty())
            continue;
        const auto found = words.find(word);
        std::string feature = found != words.end() ? found->second : word;
        std::replace(feature.begin(), feature.end(), ' ', '-');
        if ((" " + out + " ").find(" " + feature + " ") != std::string::npos)
            continue;
        instrument = instrument || feature == "instrument";
        effect = effect || feature == "audio-effect";
        out += (out.empty() ? "" : " ") + feature;
    }
    if (!instrument && !effect)
        out = "audio-effect" + std::string(out.empty() ? "" : " ") + out;
    return out;
}

// The plugins a bundle's Contents/Resources/moduleinfo.json lists (its "Audio Module Class" classes),
// described as a scan in a host would describe them, their path the bundle's. False, with the reason
// and nothing added to `found`, when there is no such file, it can't be read, or it lists no plugin:
// the bundle is then scanned in a host.
bool readModuleInfo(const std::string &bundle, std::vector<Descriptor> &found, std::string &error);

}
