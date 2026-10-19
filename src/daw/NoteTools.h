#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "Project.h"

namespace daw {

// Note editing as plain functions on a clip's notes, for the piano roll and tests. Times are beats
// from the clip's start. "Selected" means Note::selected; a tool given no selected notes works on
// every note.

// Scales: semitone steps from the root, within an octave
struct Scale {
    std::string name;
    std::vector<int> steps;
};
const std::vector<Scale> &scales(); // Major, Natural minor, ..., Chromatic
const Scale *findScale(const std::string &name);
// Whether `key` is in the scale on `root` (0 C ... 11 B)
bool inScale(int key, int root, const Scale &scale);
// The nearest key in the scale (ties go down)
int snapToScale(int key, int root, const Scale &scale);
// The scale's degree of `key` (0 the root), or -1 when it isn't in it
int scaleDegree(int key, int root, const Scale &scale);
// `degrees` scale steps above (or below) `key`, which is moved into the scale first
int stepInScale(int key, int degrees, int root, const Scale &scale);
const char *rootName(int root); // "C", "C#", ...

// Chords: semitone offsets from the root. "Diatonic" ones are built in thirds on the scale.
struct ChordShape {
    std::string name;
    std::vector<int> intervals; // empty for the diatonic ones
    int diatonicNotes = 0;      // 3 or 4 for a triad or a seventh in the scale
};
const std::vector<ChordShape> &chordShapes();
// The chord's keys on `root` (a key; in the scale for diatonic shapes)
std::vector<int> chordKeys(const ChordShape &shape, int root, int scaleRoot, const Scale &scale);

// New notes: a run up the scale (direction 1), down (-1) or up then down (0) over `octaves`, each
// `step` long, from `start`; and a chord, `length` long. Velocity for every note.
std::vector<Note> scaleRun(int fromKey, int octaves, int direction, double start, double step, float velocity,
                           int scaleRoot, const Scale &scale);
std::vector<Note> chord(const std::vector<int> &keys, double start, double length, float velocity);

// Arpeggiator: notes sounding together (chords, by overlap) become a pattern stepping through
// their keys across `octaves`, one step every `rate` beats for as long as the chord lasts (or
// `length` beats when set), each `gate` of a step long. The originals are replaced.
// - Patterns: up, down, up and down (without or with the ends repeated), down and up, converge
//   (outside in), diverge (inside out), thumb (the lowest between each of the others), pinky (the
//   highest between each, as in an Alberti bass), random, random walk (a step up or down each
//   time), as played (the order the notes start in), chord (all of them each step).
// - Octaves stack up, down, or both ways (below and above).
// - `rhythm`, steps repeating through the pattern: 'x' plays, 'X' plays accented, '-' ties the
//   step before over this one, '.' rests (a rest or tie doesn't move the pattern on); empty plays
//   every step. `repeats` plays each step that many times within it (ratchets).
// - `swing` (0-1) delays every second step, up to a triplet shuffle; `transpose` moves the
//   pattern by semitones each time it goes round; `humanize` (0-1) nudges times (a tenth of a
//   step at most) and velocities.
// - Velocity: as played, fixed (`velocity`), rising or falling across the chord; accents add
//   `accent`.
enum class ArpPattern { Up, Down, UpDown, UpAndDown, DownUp, Converge, Diverge, Thumb, Pinky, Random, RandomWalk, AsPlayed, Chord };
enum class ArpOctaves { Up, Down, Both };
enum class ArpVelocity { AsPlayed, Fixed, Rising, Falling };
struct ArpSettings {
    ArpPattern pattern = ArpPattern::Up;
    double rate = 0.25;
    int octaves = 1;
    ArpOctaves octaveMode = ArpOctaves::Up;
    double gate = 0.8;      // 0.05-2 of a step (over 1 overlaps the next)
    double swing = 0.0;     // 0-1
    int repeats = 1;        // 1-4
    std::string rhythm = "xxxxxxxx"; // "x.xX-x.." (see above); empty plays every step
    int transpose = 0;      // semitones each time round, -12 to 12
    double humanize = 0.0;  // 0-1
    ArpVelocity velocityMode = ArpVelocity::AsPlayed;
    float velocity = 0.8f;  // fixed velocity, and the top of the rises and falls
    float accent = 0.2f;    // added on accented steps
    double length = 0.0;    // beats each chord's pattern lasts; 0 as long as the chord
    uint32_t seed = 1;      // for Random, Random walk and humanize
};
const std::vector<ArpPattern> &arpPatterns(); // in the order a menu lists them
const char *arpPatternName(ArpPattern pattern);
// Named starting points for the dialog
struct ArpPreset {
    std::string name;
    ArpSettings settings;
};
const std::vector<ArpPreset> &arpPresets();
// Returns how many notes it made
int arpeggiate(std::vector<Note> &notes, const ArpSettings &settings);

// Editing. Each returns how many notes it changed.
// Starts (and, with ends, lengths) moved toward the grid by `strength` (0-1)
int quantize(std::vector<Note> &notes, double grid, double strength = 1.0, bool ends = false);
// Each note lasts until the next one starts (of any key)
int legato(std::vector<Note> &notes);
// Random nudges: starts by up to `timing` beats, velocities by up to `velocity`
int humanize(std::vector<Note> &notes, double timing, float velocity, uint32_t seed);
// Velocity: set, scaled around its middle (compress below 1, expand above), shifted, and a ramp
// from `from` to `to` across the notes in time order
int setVelocity(std::vector<Note> &notes, float velocity);
int scaleVelocity(std::vector<Note> &notes, float factor);
int shiftVelocity(std::vector<Note> &notes, float by);
int rampVelocity(std::vector<Note> &notes, float from, float to);
// Moves keys by semitones, or into the scale
int transpose(std::vector<Note> &notes, int semitones);
int fitToScale(std::vector<Note> &notes, int root, const Scale &scale);
// Times stretched around the earliest start (2 doubles them)
int stretch(std::vector<Note> &notes, double factor);
// The selection played backwards in time, or upside down around its middle key
int reverse(std::vector<Note> &notes);
int invert(std::vector<Note> &notes);
// Notes cut into pieces `grid` long; same-key notes that touch joined into one
int chop(std::vector<Note> &notes, double grid);
int join(std::vector<Note> &notes);
// Same-key overlaps trimmed so each ends where the next begins
int fixOverlaps(std::vector<Note> &notes);
// Copies of the selection placed right after it (the copies become the selection); returns how
// many
int duplicate(std::vector<Note> &notes, double grid);
// Removes the selected notes; returns how many
int removeSelected(std::vector<Note> &notes);
// Keeps notes inside the clip: starts within [0, length), ends at most `length`; drops the rest
void clampToClip(std::vector<Note> &notes, double length, double minimum);

// Muting: if any of the notes acted on plays, they are all muted; otherwise they all play again.
// Returns how many changed.
int toggleMute(std::vector<Note> &notes);
// Starts moved by `beats` together (never before 0, so the earliest stops there and the rest keep
// their spacing)
int nudge(std::vector<Note> &notes, double beats);
// Lengths changed by `beats`, at least `minimum`
int resizeBy(std::vector<Note> &notes, double beats, double minimum);
// Times scaled around `anchor` (starts and lengths; lengths at least `minimum`)
int stretchAround(std::vector<Note> &notes, double anchor, double factor, double minimum);
// Cuts note `index` in two at `at` (beats; at least `minimum` inside the note on each side). The
// right part goes at the end of the list. Returns whether it cut.
bool slice(std::vector<Note> &notes, size_t index, double at, double minimum);
// A lasso in beats (x) and keys (y). The notes whose centre line (start to end, at key + 0.5)
// has a point inside it are selected; `additive` keeps the selection there was.
struct NotePoint {
    double beat = 0.0;
    double key = 0.0;
};
bool insidePolygon(const std::vector<NotePoint> &polygon, NotePoint p);
int selectInLasso(std::vector<Note> &notes, const std::vector<NotePoint> &polygon, bool additive);
// Adds every note on a key a selected note has; returns how many
int selectSameKeys(std::vector<Note> &notes);
// Moves the selection to the next (1) or previous (-1) note in time (then by key); with none
// selected, the first or last. Returns the index selected, or -1 when there are no notes.
int selectNext(std::vector<Note> &notes, int direction);
// The span of the selected notes (or all, when none are); count 0 when there are none
struct NoteBounds {
    double start = 0.0, end = 0.0;
    int low = 0, high = 0;
    int count = 0;
};
NoteBounds noteBounds(const std::vector<Note> &notes);

// The grid that fits a zoom: the finest of 4, 2, 1, 1/2 ... 1/64 beats (times 2/3 for triplets)
// whose lines stay at least `minPixels` apart
double autoGrid(double pixelsPerBeat, double minPixels, bool triplet);
// A grid's name: "1/16", "1/8T", "1 bar" (in a bar of `beatsPerBar` beats)
std::string gridName(double beats, int beatsPerBar);

// Controller lanes (see ControlLane): the clip's lane for a controller (null when it has none),
// or one made for it
ControlLane *findLane(std::vector<ControlLane> &lanes, int controller);
ControlLane &laneFor(std::vector<ControlLane> &lanes, int controller);
// A point at `beat`, replacing any within `merge` beats of it; points stay sorted
void setControlPoint(ControlLane &lane, double beat, float value, double merge);
// Removes the points from `from` to `to` (inclusive); returns how many
int eraseControlPoints(ControlLane &lane, double from, double to);
// A straight line: the points between replaced by its two ends
void controlLine(ControlLane &lane, double from, float fromValue, double to, float toValue);
// Drops points a straight line through their neighbours already passes within `tolerance` of
// (after painting); stepped lanes drop only repeats. Returns how many went.
int thinControlPoints(ControlLane &lane, float tolerance);
// Lanes with no points go
void dropEmptyLanes(std::vector<ControlLane> &lanes);

// Selection helpers
int selectedCount(const std::vector<Note> &notes);
void selectAll(std::vector<Note> &notes, bool selected = true);
// The selected notes (or all, when none are), as copies with starts relative to the first
std::vector<Note> copySelection(const std::vector<Note> &notes);
// Adds copies at `at`, selected (the others deselected)
void paste(std::vector<Note> &notes, const std::vector<Note> &clipboard, double at);

}
