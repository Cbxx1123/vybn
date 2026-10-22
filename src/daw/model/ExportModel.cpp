#include "daw/model/ExportModel.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <sstream>
#include <utility>

#include "daw/model/Project.h"

namespace daw {

bool ExportTarget::operator==(const ExportTarget &o) const
{
    return enabled == o.enabled && name == o.name && source == o.source && tracks == o.tracks && tap == o.tap &&
           respectMuteSolo == o.respectMuteSolo && range == o.range && fromBar == o.fromBar && toBar == o.toBar &&
           tailSeconds == o.tailSeconds && format == o.format && bits == o.bits && bitrate == o.bitrate && sampleRate == o.sampleRate &&
           mono == o.mono && normalize == o.normalize && peakDb == o.peakDb && loudnessLufs == o.loudnessLufs &&
           limiter == o.limiter && ceilingDbtp == o.ceilingDbtp && fadeInMs == o.fadeInMs && fadeOutMs == o.fadeOutMs &&
           dither == o.dither && fileName == o.fileName && folder == o.folder;
}

bool ExportPreset::operator==(const ExportPreset &o) const
{
    return name == o.name && targets == o.targets;
}

bool ExportMetadata::operator==(const ExportMetadata &o) const
{
    return title == o.title && artist == o.artist && album == o.album && comment == o.comment;
}

std::vector<ExportPreset> defaultExportPresets()
{
    ExportTarget mix;
    mix.name = "Mix";
    mix.source = ExportSource::Master;
    mix.format = ExportFormat::Wav;
    mix.bits = 24;
    mix.sampleRate = 48000;

    // Loudness-normalized masters, under the limiter's ceiling
    ExportTarget masterWav;
    masterWav.name = "Master WAV";
    masterWav.source = ExportSource::Master;
    masterWav.format = ExportFormat::Wav;
    masterWav.bits = 24;
    masterWav.normalize = NormalizeMode::Loudness;
    masterWav.loudnessLufs = -14.0;
    masterWav.limiter = true;
    masterWav.ceilingDbtp = -1.0;
    masterWav.dither = DitherMode::Tpdf;
    // The names differ by extension between the WAV and the FLAC (the templates never repeat)
    masterWav.fileName = "{project} - Release";

    ExportTarget masterFlac = masterWav;
    masterFlac.name = "Master FLAC";
    masterFlac.format = ExportFormat::Flac;
    masterFlac.bits = 16;
    masterFlac.dither = DitherMode::NoiseShaped;

    // Each track as it is, post-fader, whatever is muted or soloed, and the master beside them. The
    // tracks' names come from {track}; the master's file is named apart from the Release's.
    ExportTarget stems;
    stems.name = "Stems";
    stems.source = ExportSource::EachTrack;
    stems.format = ExportFormat::Wav;
    stems.bits = 24;
    stems.respectMuteSolo = false;
    stems.fileName = "{project} - {track}";

    ExportTarget master;
    master.name = "Master";
    master.source = ExportSource::Master;
    master.fileName = "{project} - Stems master";

    ExportTarget midi;
    midi.name = "MIDI";
    midi.source = ExportSource::Midi;

    return {
        {"Mix", {mix}},
        {"Release", {masterWav, masterFlac}},
        {"Stems", {stems, master}},
        {"MIDI", {midi}},
    };
}

namespace {

template <typename E>
using NameTable = std::pair<E, const char *>;

const NameTable<ExportSource> kSources[] = {
    {ExportSource::Master, "master"},
    {ExportSource::MasterDry, "master_dry"},
    {ExportSource::EachTrack, "each_track"},
    {ExportSource::Tracks, "tracks"},
    {ExportSource::Midi, "midi"},
};
const NameTable<ExportTap> kTaps[] = {
    {ExportTap::PostFader, "post_fader"},
    {ExportTap::PreFader, "pre_fader"},
};
const NameTable<ExportRangeKind> kRanges[] = {
    {ExportRangeKind::Song, "song"},
    {ExportRangeKind::Timeline, "timeline"},
    {ExportRangeKind::Loop, "loop"},
    {ExportRangeKind::Bars, "bars"},
};
const NameTable<ExportFormat> kFormats[] = {
    {ExportFormat::Wav, "wav"},
    {ExportFormat::Aiff, "aiff"},
    {ExportFormat::Flac, "flac"},
    {ExportFormat::Mp3, "mp3"},
};
const NameTable<NormalizeMode> kNormalize[] = {
    {NormalizeMode::Off, "off"},
    {NormalizeMode::Peak, "peak"},
    {NormalizeMode::Loudness, "loudness"},
};
const NameTable<DitherMode> kDithers[] = {
    {DitherMode::None, "none"},
    {DitherMode::Tpdf, "tpdf"},
    {DitherMode::NoiseShaped, "noise_shaped"},
};

template <typename E, size_t N>
const char *nameOf(E value, const NameTable<E> (&table)[N])
{
    for (const auto &entry : table) {
        if (entry.first == value)
            return entry.second;
    }
    return "";
}

template <typename E, size_t N>
bool lookup(const std::string &name, const NameTable<E> (&table)[N], E &out)
{
    for (const auto &entry : table) {
        if (name == entry.second) {
            out = entry.first;
            return true;
        }
    }
    return false;
}

// Characters Windows doesn't allow in a file name, and control characters, become "_"
std::string safeFileText(const std::string &text)
{
    std::string out = text;
    for (char &ch : out) {
        const unsigned char code = static_cast<unsigned char>(ch);
        if (code < 32 || std::strchr("<>:\"/\\|?*", ch))
            ch = '_';
    }
    return out;
}

bool endsWithIgnoringCase(const std::string &text, const std::string &suffix)
{
    if (text.size() < suffix.size())
        return false;
    for (size_t i = 0; i < suffix.size(); ++i) {
        const char a = static_cast<char>(std::tolower(static_cast<unsigned char>(text[text.size() - suffix.size() + i])));
        const char b = static_cast<char>(std::tolower(static_cast<unsigned char>(suffix[i])));
        if (a != b)
            return false;
    }
    return true;
}

// The extension a target's file gets
std::string extensionOf(const ExportTarget &target)
{
    if (target.source == ExportSource::Midi)
        return ".mid";
    switch (target.format) {
    case ExportFormat::Aiff: return ".aiff";
    case ExportFormat::Flac: return ".flac";
    case ExportFormat::Mp3: return ".mp3";
    case ExportFormat::Wav: break;
    }
    return ".wav";
}

std::string formatText(const ExportTarget &target)
{
    if (target.source == ExportSource::Midi)
        return "mid";
    return enumName(target.format);
}

// 48000 is "48k", 44100 "44.1k"
std::string rateText(uint32_t rate)
{
    std::ostringstream out;
    out << rate / 1000.0 << "k";
    return out.str();
}

std::string dateText(std::time_t now, const char *format)
{
    const std::tm *local = std::localtime(&now);
    if (!local)
        return {};
    char buffer[32];
    const size_t written = std::strftime(buffer, sizeof buffer, format, local);
    return std::string(buffer, written);
}

// Replaces each {token} the expansion knows; anything else is kept as written
std::string expand(const std::string &templ, const ExportTarget &target, const Project &project,
                   const std::string &presetName, const std::string &trackName, std::time_t now)
{
    std::string out;
    size_t i = 0;
    while (i < templ.size()) {
        const size_t open = templ.find('{', i);
        const size_t close = open == std::string::npos ? std::string::npos : templ.find('}', open);
        if (close == std::string::npos) {
            out += templ.substr(i);
            break;
        }
        out += templ.substr(i, open - i);
        const std::string token = templ.substr(open + 1, close - open - 1);
        if (token == "project")
            out += project.name.empty() ? std::string("Untitled") : project.name;
        else if (token == "target")
            out += target.name;
        else if (token == "track")
            out += trackName;
        else if (token == "date")
            out += dateText(now, "%Y-%m-%d");
        else if (token == "time")
            out += dateText(now, "%H-%M");
        else if (token == "rate")
            out += rateText(target.format == ExportFormat::Mp3 ? mp3RateFor(target.sampleRate) : target.sampleRate);
        else if (token == "bits")
            out += target.format == ExportFormat::Mp3 ? std::to_string(target.bitrate) + "kbps" : std::to_string(target.bits);
        else if (token == "format")
            out += formatText(target);
        else if (token == "preset")
            out += presetName;
        else
            out += templ.substr(open, close - open + 1);
        i = close + 1;
    }
    return out;
}

}

const char *enumName(ExportSource source) { return nameOf(source, kSources); }
const char *enumName(ExportTap tap) { return nameOf(tap, kTaps); }
const char *enumName(ExportRangeKind range) { return nameOf(range, kRanges); }
const char *enumName(ExportFormat format) { return nameOf(format, kFormats); }
const char *enumName(NormalizeMode mode) { return nameOf(mode, kNormalize); }
const char *enumName(DitherMode mode) { return nameOf(mode, kDithers); }

bool fromName(const std::string &name, ExportSource &out) { return lookup(name, kSources, out); }
bool fromName(const std::string &name, ExportTap &out) { return lookup(name, kTaps, out); }
bool fromName(const std::string &name, ExportRangeKind &out) { return lookup(name, kRanges, out); }
bool fromName(const std::string &name, ExportFormat &out) { return lookup(name, kFormats, out); }
bool fromName(const std::string &name, NormalizeMode &out) { return lookup(name, kNormalize, out); }
bool fromName(const std::string &name, DitherMode &out) { return lookup(name, kDithers, out); }

std::string resolveFileName(const ExportTarget &target, const Project &project, const std::string &presetName,
                            const std::string &trackName, std::time_t now)
{
    std::string name = expand(target.fileName, target, project, presetName, trackName, now);
    if (target.source == ExportSource::EachTrack && !trackName.empty() &&
        target.fileName.find("{track}") == std::string::npos)
        name += " - " + trackName;
    name = safeFileText(name);
    if (name.empty())
        name = safeFileText(project.name.empty() ? std::string("Untitled") : project.name);
    const std::string extension = extensionOf(target);
    if (!endsWithIgnoringCase(name, extension))
        name += extension;
    return name;
}

uint32_t mp3RateFor(uint32_t rate)
{
    if (rate == 32000)
        return rate;
    return rate % 11025 == 0 ? 44100u : 48000u;
}

bool exportTargetRange(const Project &project, const ExportTarget &target, double &startBeat, double &endBeat)
{
    const audio::TempoMap &timing = project.timing;
    switch (target.range) {
    case ExportRangeKind::Song: {
        double end = 0.0;
        for (const Track &t : project.tracks) {
            for (const Clip &c : t.clips)
                end = std::max(end, c.start + c.length);
        }
        startBeat = 0.0;
        endBeat = std::max(end, timing.barStart(1));
        return true;
    }
    case ExportRangeKind::Timeline:
        startBeat = 0.0;
        endBeat = project.lengthBeats();
        return true;
    case ExportRangeKind::Loop:
        if (!project.hasLoop())
            return false;
        startBeat = project.loopStart;
        endBeat = project.loopEnd;
        return true;
    case ExportRangeKind::Bars:
        startBeat = std::max(0.0, timing.barStart(static_cast<int>(std::lround(target.fromBar)) - 1));
        endBeat = std::max(0.0, timing.barStart(static_cast<int>(std::lround(target.toBar)) - 1));
        return endBeat > startBeat;
    }
    return false;
}

}
