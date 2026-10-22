#pragma once

#include <cstdint>
#include <ctime>
#include <string>
#include <vector>

namespace daw {

struct Project;

// What an export writes: a model of the targets (what to render, how, and where), saved with the
// project as presets. Rendering and encoding are elsewhere; this file only describes them, names
// their files and says which part of the song they cover.

// What a target takes from the mix
enum class ExportSource {
    Master,    // the master output, with the master effects and gain
    MasterDry, // every track summed, without the master effects
    EachTrack, // one output per track
    Tracks,    // the chosen tracks summed
    Midi,      // a Standard MIDI File of the instrument tracks' notes (no audio)
};
// Whether a track's taken after its fader and pan (post) or before them (pre)
enum class ExportTap { PostFader, PreFader };
// Which part of the song: to the last clip's end, the whole timeline, the project's loop, or bars
enum class ExportRangeKind { Song, Timeline, Loop, Bars };
enum class ExportFormat { Wav, Aiff, Flac, Mp3 };
enum class NormalizeMode { Off, Peak, Loudness };
enum class DitherMode { None, Tpdf, NoiseShaped };

// One output file of a preset
struct ExportTarget {
    bool enabled = true;
    std::string name = "Mix";
    ExportSource source = ExportSource::Master;
    std::vector<std::string> tracks; // by track name (stable across reorders)
    ExportTap tap = ExportTap::PostFader;
    bool respectMuteSolo = true;
    ExportRangeKind range = ExportRangeKind::Song;
    double fromBar = 1, toBar = 9; // Bars: 1-based, toBar is exclusive
    double tailSeconds = 4.0;      // how long after the range notes may ring on
    ExportFormat format = ExportFormat::Wav;
    int bits = 24;           // WAV, AIFF and FLAC (an MP3 is encoded from 16 bits)
    int bitrate = 320;       // MP3 only, in kbps: one of audio::kMp3Bitrates
    uint32_t sampleRate = 48000;
    bool mono = false;
    NormalizeMode normalize = NormalizeMode::Off;
    double peakDb = -0.3;
    double loudnessLufs = -14.0;
    bool limiter = true;
    double ceilingDbtp = -1.0;
    double fadeInMs = 0, fadeOutMs = 0;
    DitherMode dither = DitherMode::Tpdf; // used for 16 and 24 bits
    // Template: {project} {target} {track} {date} {time} {rate} {bits} {format} {preset} ({bits} is
    // an MP3's bitrate, as "320kbps")
    std::string fileName = "{project}";
    std::string folder; // empty: beside the project (else the library's Exports folder)

    bool operator==(const ExportTarget &other) const;
    bool operator!=(const ExportTarget &other) const { return !(*this == other); }
};

// A named list of targets, rendered together by one press of Export
struct ExportPreset {
    std::string name;
    std::vector<ExportTarget> targets;

    bool operator==(const ExportPreset &other) const;
    bool operator!=(const ExportPreset &other) const { return !(*this == other); }
};

// Written into the files' metadata; an empty title means the project's name
struct ExportMetadata {
    std::string title, artist, album, comment;

    bool operator==(const ExportMetadata &other) const;
    bool operator!=(const ExportMetadata &other) const { return !(*this == other); }
};

// The presets a new or demo project starts with: "Mix", "Release" (a loudness-normalized WAV and
// a noise-shaped 16-bit FLAC), "Stems" and "MIDI"
std::vector<ExportPreset> defaultExportPresets();

// Names as they are written in project files ("master_dry", "post_fader", "noise_shaped")
const char *enumName(ExportSource source);
const char *enumName(ExportTap tap);
const char *enumName(ExportRangeKind range);
const char *enumName(ExportFormat format);
const char *enumName(NormalizeMode mode);
const char *enumName(DitherMode mode);
// Reads a name back; false (and `out` unchanged) for a name that isn't one of them
bool fromName(const std::string &name, ExportSource &out);
bool fromName(const std::string &name, ExportTap &out);
bool fromName(const std::string &name, ExportRangeKind &out);
bool fromName(const std::string &name, ExportFormat &out);
bool fromName(const std::string &name, NormalizeMode &out);
bool fromName(const std::string &name, DitherMode &out);

// The file a target writes, from its name template: the tokens are expanded, characters Windows
// doesn't allow in file names become "_", and the extension is added unless the name has it.
// An EachTrack target whose template has no {track} gets " - <track>" after the name. `trackName`
// is the track of an EachTrack output (empty for the others); `presetName` is the preset's name.
std::string resolveFileName(const ExportTarget &target, const Project &project, const std::string &presetName,
                            const std::string &trackName, std::time_t now);

// The sample rate an MP3 is written at for a target set to `rate`: 32, 44.1 or 48 kHz, keeping the
// family (44.1 kHz and its multiples become 44.1 kHz, the others 48 kHz unless it is 32 kHz)
uint32_t mp3RateFor(uint32_t rate);

// The part of the song a target covers, in beats: Song is 0 to the last clip's end (at least one
// bar), Timeline the whole timeline, Loop the project's loop (false when there is none), Bars the
// bars from fromBar to toBar (false when that's empty)
bool exportTargetRange(const Project &project, const ExportTarget &target, double &startBeat, double &endBeat);

}
