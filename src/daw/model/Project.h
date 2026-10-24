#pragma once

#include <cstdint>

#include <string>
#include <utility>
#include <vector>

#include "daw/model/ExportModel.h"
#include "audio/Device.h"
#include "audio/TempoMap.h"
#include "audio/Warp.h"

namespace daw {

// The session the DAW edits: tracks with clips on a beat timeline. DawApp mirrors each track's
// gain, pan, mute and solo into an audio::TrackProcessor, plays instrument tracks' notes through
// their instrument and audio tracks' clips from their sound files. Positions and lengths are in
// beats (quarter notes), turned into time and bars by `timing`; times inside sound files are in
// seconds (audio isn't stretched by the tempo).

// A point of a curve: a controller's in an instrument clip (beats from the clip's start, values
// 0-1), or a note's expression (beats from the note's start, in the expression's units)
constexpr int kPitchBend = 128; // the lane for the pitch bend, beside MIDI's controllers 0-127
struct ControlPoint {
    double beat = 0.0;
    float value = 0.0f;
};

// What a single note can change while it sounds. Tuning is in semitones (-48 to 48). Saved as
// words ("tuning"), so the order here can change.
enum class Expression : uint8_t { Tuning };
constexpr int kExpressions = 1;
const char *expressionKey(Expression type);       // "tuning"
bool expressionFromKey(const std::string &key, Expression &type);
std::string expressionName(Expression type);      // "Note tuning"
float expressionNeutral(Expression type);         // the value of a note without the curve
float expressionMin(Expression type);
float expressionMax(Expression type);

// A note's expression over its length: points in beats from its start (sorted), joined by straight
// lines; before the first point its value, after the last the last's. Points past the note's end
// are kept (so shortening and lengthening a note gives them back) but never play.
struct NoteCurve {
    Expression type = Expression::Tuning;
    std::vector<ControlPoint> points;

    float valueAt(double beat) const;
};
bool operator==(const NoteCurve &a, const NoteCurve &b);
inline bool operator!=(const NoteCurve &a, const NoteCurve &b) { return !(a == b); }

// A MIDI note in a clip, positioned from the clip's start
struct Note {
    double start = 0.0;
    double length = 1.0;
    int key = 60;          // MIDI note number; 60 is middle C (C4)
    float velocity = 0.8f; // 0-1
    bool muted = false;    // kept in the clip but not played
    bool selected = false; // in the piano roll's selection; not saved
    std::vector<NoteCurve> expression = {}; // at most one curve per type; empty for most notes

    const NoteCurve *curve(Expression type) const;
    // The curve's value, or the type's neutral value without one
    float expressionAt(Expression type, double beat) const;
    // Sets the curve's points (empty removes it)
    void setCurve(Expression type, std::vector<ControlPoint> points);
};

// A note's curves over [from, to) of it (beats from its start), moved to start at `from`: the
// points inside, and a point at each cut holding the value there so nothing jumps. A curve whose
// part is all at the neutral value is left out. For cutting a note in two (`to` infinite: the rest).
std::vector<NoteCurve> cutCurves(const std::vector<NoteCurve> &curves, double from, double to);
// `b`'s curves joined onto `a`'s at `offset` beats into `a` (a's points from there on go): for
// joining a note that starts there onto `a`
std::vector<NoteCurve> joinCurves(const std::vector<NoteCurve> &a, const std::vector<NoteCurve> &b, double offset);
// Times scaled by `factor`
void stretchCurves(std::vector<NoteCurve> &curves, double factor);
// Played backwards over a note `length` long (points past its end go first)
void reverseCurves(std::vector<NoteCurve> &curves, double length);

// A controller's curve in an instrument clip: points in beats from the clip's start (sorted),
// values 0-1, joined by straight lines (held, for switches such as the sustain pedal)
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

// A track's or a clip's identity in its project: never 0 once Project::normalizeIds has run, unique
// among the project's tracks (and among all its clips), kept through edits, undo, saving and
// opening, and never reused within a session (ProjectStore keeps the high-water mark). Copies made
// by editing (a duplicated or split clip) start as duplicates and get a fresh id from
// normalizeIds, the first in the project keeping the old one. Ids are bookkeeping: they are saved,
// but sameContent, sameSaved and the audio hashes ignore them.
using ObjectId = uint32_t;
constexpr ObjectId kNoId = 0;

struct Clip {
    Clip() = default;
    Clip(std::string clipName, double clipStart, double clipLength, std::vector<Note> clipNotes = {})
        : name(std::move(clipName)), start(clipStart), length(clipLength), notes(std::move(clipNotes))
    {
    }

    ObjectId id = kNoId;
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
    // Warping (daw/model/Warp.h): with warp.on the sound follows the bars through its map, `offset`
    // still the file's seconds at the clip's start
    audio::Warp warp;

    // In the track's own lane, a comp section: the take (a clip in one of the track's take lanes)
    // it was cut from (docs/design/takes-plan.md). kNoId for an ordinary clip, and in a take lane.
    ObjectId take = kNoId;

    bool hasAudio() const { return !audioFile.empty(); }
    double end() const { return start + length; }
};

// Recorded passes kept under a track, which don't play: the track's comp (its own clips) is cut
// from them (Takes.h). A lane's clips don't overlap, sorted by start.
struct TakeLane {
    ObjectId id = kNoId; // from the same counter as clip ids, unique among them
    std::string name;
    std::vector<Clip> clips;
};

// Audio and instrument tracks make sound; return and group tracks (buses) take other tracks' instead:
// a return what the tracks send it, a group what the tracks whose output it is play (docs/design/routing-plan.md)
enum class TrackKind { Audio, Instrument, Return, Group };
inline bool isBus(TrackKind kind) { return kind == TrackKind::Return || kind == TrackKind::Group; }

// A share of a track's sound sent to a return track, before its fader (and pan) or after them
struct Send {
    ObjectId to = kNoId;  // the bus: a return or group track
    float level = 1.0f;   // linear, 0-1 (0 dB at most)
    bool preFader = false;

    bool operator==(const Send &o) const { return to == o.to && level == o.level && preFader == o.preFader; }
    bool operator!=(const Send &o) const { return !(*this == o); }
};

struct Track {
    ObjectId id = kNoId;
    std::string name;
    TrackKind kind = TrackKind::Audio;
    std::vector<Clip> clips;
    float gain = 1.0f; // linear, 0-2 (+6 dB)
    float pan = 0.0f;  // -1 left to +1 right
    bool mute = false;
    bool solo = false;
    bool armed = false;
    // Groups: whether the tracks that go into it are folded away in the arrangement and the mixer. Saved
    // with the project but, like arming, not an undo step (never set on any other kind of track).
    bool folded = false;
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
    // Automation of the track's own volume and pan (audio::kVolumeParam, kPanParam); its devices'
    // parameters have theirs in their settings. Sends' levels have theirs here too (sendParam).
    std::vector<audio::AutomationLane> automation;
    // Where the track's sound goes: a group track (its id), or the master (kNoId); and the shares of
    // it sent to return tracks, one send per return at most. Mix fields (editMix), like the gain.
    ObjectId output = kNoId;
    std::vector<Send> sends;
    // Audio and instrument tracks: the takes recorded on it, newest first. Content, like clips.
    std::vector<TakeLane> takeLanes;

    bool bus() const { return isBus(kind); }
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

// A named place in the song (docs/design/markers-and-import-plan.md)
struct Marker {
    ObjectId id = kNoId; // from the clip counter (normalizeIds); bookkeeping, like a clip's
    double beat = 0.0;
    std::string name;

    bool operator==(const Marker &o) const { return beat == o.beat && name == o.name; }
    bool operator!=(const Marker &o) const { return !(*this == o); }
};

struct Project {
    std::string name = "Untitled";
    std::vector<Track> tracks;
    // The tempo and time signatures over the song; DawApp hands each change to the transport
    audio::TempoMap timing;
    int bars = 64; // length of the timeline
    // The loop, in beats (none while the end isn't after the start), and whether playback loops
    // in it. Saved, but not part of the undo history (like the transport).
    double loopStart = 0.0, loopEnd = 0.0;
    bool looping = false;
    bool hasLoop() const { return loopEnd > loopStart; }
    // The markers, sorted by beat. Saved and undone (the loop isn't).
    std::vector<Marker> markers;
    // The marker at or before `beat` (-1 for none), and the next one after it (-1 for none)
    int markerAt(double beat) const;
    int markerAfter(double beat) const;
    // Sorts the markers by beat, the order they were in kept for markers on one beat
    void sortMarkers();
    // Looping on or off; with no loop set, on over the selected clip (else the first four bars)
    void toggleLoop();
    // Effects on the whole mix, in order, before the master volume
    std::vector<audio::DeviceSetting> masterEffects;
    // The master's fader: linear gain 0-2 (+6 dB), after the master effects. Part of the mix (saved,
    // undone, exported), unlike the audio system's monitoring gain.
    float masterVolume = 1.0f;
    // The master volume's automation (only audio::kVolumeParam; a lane with no points plays nothing)
    std::vector<audio::AutomationLane> masterAutomation;
    // The export presets (saved with the project; what the export dialog renders) and the details
    // written into the exported files' metadata
    std::vector<ExportPreset> exportPresets;
    ExportMetadata exportMetadata;

    // The selection, shared by the arrangement, the mixer and the inspector. -1 for none.
    int selectedTrack = -1;
    int selectedClip = -1; // index into the selected track's clips

    Track *track(int index);
    const Track *track(int index) const;
    // The track with this id: its index (-1 when none), or the track (nullptr when none)
    int trackIndex(ObjectId id) const;
    Track *findTrack(ObjectId id);
    const Track *findTrack(ObjectId id) const;
    // Gives every track and clip a usable id: 0, and every duplicate after the first (in project
    // order: tracks first, then each track's clips in order; clip ids are unique across the whole
    // project), get fresh ids from `next`, which is first raised above every id in use and is left
    // above every id given. Returns whether any id changed. Ids already unique are never changed.
    bool normalizeIds(ObjectId &next);
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
    // Removes a track and its clips; the selection moves to a neighbour. The sends into it go, and
    // the tracks whose output it was go to the master.
    void deleteTrack(int index);
    // Moves a track to another index (its clips go with it); the selection follows the track it was on
    void moveTrack(int from, int to);
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
    double lengthBeats() const { return timing.barStart(bars); }
    // The beats `seconds` of sound last from `start` (an audio clip's length, which the tempo
    // doesn't stretch)
    double beatsFor(double start, double seconds) const { return timing.beatAt(timing.secondsAt(start) + seconds) - start; }
};

// The tracks' ids and kinds in order (one number each). Views that make something per track
// rebuild only when this changes, not on every edit() (which moves structureRevision for any edit)
std::vector<uint64_t> trackShape(const Project &project);

// A starting session with a few tracks and clips, shown when the app opens
Project demoProject();
// What Project > New starts from: the settings chosen, with an instrument track playing
// `instrument` (DeviceKind::None for none) and an empty clip, and an audio track, each if asked for
struct NewProjectOptions {
    std::string name = "Untitled";
    double tempo = 120.0;
    int beatsPerBar = 4;
    int beatUnit = 4; // the signature's denominator
    int bars = 32;
    bool instrumentTrack = true;
    bool audioTrack = true;
};
Project newProject(const audio::DeviceSetting &instrument, const NewProjectOptions &options = {});

const char *kindName(TrackKind kind);

}
