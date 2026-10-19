#pragma once

#include <string>
#include <utility>
#include <vector>

#include "ExportModel.h"
#include "audio/Device.h"

namespace daw {

// The session the DAW edits: tracks with clips on a beat timeline. DawApp mirrors each track's
// gain, pan, mute and solo into an audio::TrackProcessor, plays instrument tracks' notes through
// their instrument and audio tracks' clips from their sound files. Positions and lengths are in
// beats; times inside sound files are in seconds (audio isn't stretched by the tempo).

// A MIDI note in a clip, positioned from the clip's start
struct Note {
    double start = 0.0;
    double length = 1.0;
    int key = 60;          // MIDI note number; 60 is middle C (C4)
    float velocity = 0.8f; // 0-1
    bool muted = false;    // kept in the clip but not played
    bool selected = false; // in the piano roll's selection; not saved
};

// A controller's curve in an instrument clip: points in beats from the clip's start (sorted),
// values 0-1, joined by straight lines (held, for switches such as the sustain pedal)
constexpr int kPitchBend = 128; // the lane for the pitch bend, beside MIDI's controllers 0-127
struct ControlPoint {
    double beat = 0.0;
    float value = 0.0f;
};
struct ControlLane {
    int controller = 1;
    std::vector<ControlPoint> points;

    // Before the first point, its value; with no points, the controller's default
    float valueAt(double beat) const;
};
// "Mod wheel", "Sustain", "Pitch bend", "CC 20"
std::string controllerName(int controller);
// Switches (64-69: sustain, portamento, sostenuto, soft, legato, hold 2) hold until the next point
bool controllerStepped(int controller);
// Where a controller rests: the bend and pan in the middle, volume at 100, expression full, else 0
float controllerDefault(int controller);

struct Clip {
    Clip() = default;
    Clip(std::string clipName, double clipStart, double clipLength, std::vector<Note> clipNotes = {})
        : name(std::move(clipName)), start(clipStart), length(clipLength), notes(std::move(clipNotes))
    {
    }

    std::string name;
    double start = 0.0;
    double length = 4.0;
    std::vector<Note> notes; // instrument tracks only; notes past the clip's end don't play
    std::vector<ControlLane> controls; // instrument tracks only: one lane per controller used

    // Audio tracks: the sound file it plays (empty for an empty clip), where in the file the
    // clip's start is (trimming the front moves it), its level, and its fades
    std::string audioFile;
    double offset = 0.0;   // seconds into the file
    float gain = 1.0f;     // linear, 0-4
    double fadeIn = 0.0;   // seconds
    double fadeOut = 0.0;

    bool hasAudio() const { return !audioFile.empty(); }
};

enum class TrackKind { Audio, Instrument };

struct Track {
    std::string name;
    TrackKind kind = TrackKind::Audio;
    std::vector<Clip> clips;
    float gain = 1.0f; // linear, 0-2 (+6 dB)
    float pan = 0.0f;  // -1 left to +1 right
    bool mute = false;
    bool solo = false;
    bool armed = false;
    // Instrument tracks: the device that plays the notes, first in the track's chain (a library
    // instrument on the sampler, or a plugin; DeviceKind::None for none). They hear MIDI keyboards
    // while armed (or while selected, when no track is armed).
    audio::DeviceSetting instrument;
    bool hasInstrument() const { return !instrument.empty(); }
    // Audio tracks: the input channel recorded (0 is the first) and whether the next one is its
    // right side, and whether to hear the input through the track while armed
    int inputChannel = 0;
    bool inputStereo = false;
    bool monitor = false;
    // The effect devices the track's sound goes through, in order, before its gain and pan
    std::vector<audio::DeviceSetting> effects;
};

// Where a device is: in the master's chain, or a track's, at `index` among its effects, or the
// track's instrument (kInstrument)
struct DevicePlace {
    static constexpr int kInstrument = -1;
    bool master = false;
    int track = -1;
    int index = kInstrument;

    static DevicePlace instrumentOf(int track) { return {false, track, kInstrument}; }
    static DevicePlace effect(bool master, int track, int index) { return {master, master ? -1 : track, index}; }
    bool instrument() const { return !master && index == kInstrument; }
    bool operator==(const DevicePlace &o) const { return master == o.master && track == o.track && index == o.index; }
    bool operator!=(const DevicePlace &o) const { return !(*this == o); }
    std::string key() const { return (master ? std::string("M") : "T" + std::to_string(track)) + ":" + std::to_string(index); }
};

struct Project {
    std::string name = "Untitled";
    std::vector<Track> tracks;
    double tempo = 120.0; // BPM; DawApp keeps it in step with the transport
    double secondsPerBeat() const { return 60.0 / tempo; }
    int beatsPerBar = 4;
    int bars = 64; // length of the timeline
    // The loop, in beats (none while the end isn't after the start), and whether playback loops
    // in it. Saved, but not part of the undo history (like the transport).
    double loopStart = 0.0, loopEnd = 0.0;
    bool looping = false;
    bool hasLoop() const { return loopEnd > loopStart; }
    // Looping on or off; with no loop set, on over the selected clip (else the first four bars)
    void toggleLoop();
    // Effects on the whole mix, in order, before the master gain
    std::vector<audio::DeviceSetting> masterEffects;
    // The export presets (saved with the project; what the export dialog renders) and the details
    // written into the exported files' metadata
    std::vector<ExportPreset> exportPresets;
    ExportMetadata exportMetadata;

    // The selection, shared by the arrangement, the mixer and the inspector. -1 for none.
    int selectedTrack = -1;
    int selectedClip = -1; // index into the selected track's clips

    Track *track(int index);
    const Track *track(int index) const;
    // A chain's effects (the master's, or a track's; nullptr for no such track), and the device at a
    // place (nullptr when there is none: a track without an instrument, an index past the end)
    std::vector<audio::DeviceSetting> *effects(bool master, int track);
    const std::vector<audio::DeviceSetting> *effects(bool master, int track) const;
    audio::DeviceSetting *device(const DevicePlace &place);
    const audio::DeviceSetting *device(const DevicePlace &place) const;
    Clip *clip(); // the selected clip, or nullptr
    const Clip *clip() const; // the selected clip, or nullptr
    void select(int track, int clip = -1);
    // Appends a track and selects it; returns its index
    int addTrack(TrackKind kind);
    // Removes a track and its clips; the selection moves to a neighbour
    void deleteTrack(int index);
    // Removes the selected clip, if any
    void deleteClip();
    // Cuts the selected clip in two at `beat` (on the timeline), keeping the left part selected.
    // Notes go to the part they start in; the right part of an audio clip reads on from where the
    // cut was. Returns false if the beat isn't inside the clip.
    bool splitClip(double beat);
    // Moves a clip's start edge to `beat`, keeping what it plays where it was: the audio offset
    // and the notes move against it. Clamped to keep at least `minimum` beats, and so an audio
    // clip doesn't start before its file does.
    void trimStart(Clip &clip, double beat, double minimum = 0.25) const;
    // Copies the selected clip to just after itself and selects the copy
    void duplicateClip();
    double lengthBeats() const { return static_cast<double>(bars) * beatsPerBar; }
};

// A starting session with a few tracks and clips, shown when the app opens
Project demoProject();
// What Project > New starts from: the settings chosen, with an instrument track playing
// `instrument` (DeviceKind::None for none) and an empty clip, and an audio track, each if asked for
struct NewProjectOptions {
    std::string name = "Untitled";
    double tempo = 120.0;
    int beatsPerBar = 4;
    int bars = 32;
    bool instrumentTrack = true;
    bool audioTrack = true;
};
Project newProject(const audio::DeviceSetting &instrument, const NewProjectOptions &options = {});

const char *kindName(TrackKind kind);

}
