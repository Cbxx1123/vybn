#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "Project.h"

namespace daw {

// The step sequencer's plain logic: an instrument clip's notes seen as a grid of rows (one key
// each) by steps, for StepSequencerView and tests. Nothing here is saved: a step is on while a note
// of the row's key starts inside it, so the piano roll, undo, saving, playback and the exports all
// see ordinary notes. Times are beats from the clip's start, like Note::start.
//
// Every function that changes `notes` leaves them sorted by start (a stable sort), never adds
// selected notes, and leaves notes of other keys, and of this key outside the grid, untouched.

// The grid over one clip
struct StepGrid {
    double step = 0.25; // beats a step (0.25 is a 1/16 in 4/4)
    int count = 16;     // whole steps that fit in the clip, at least 1
    double swing = 0.0; // 0-1: every odd step (1, 3, 5...) plays up to a third of a step late
};

// The step sizes offered: "1/8", "1/16", "1/32", "1/8T", "1/16T" (beats 0.5, 0.25, 0.125, 1/3, 1/6)
struct StepChoice {
    const char *name;
    double beats;
};
const std::vector<StepChoice> &stepChoices();
constexpr int kDefaultStepChoice = 1; // 1/16

// The grid for a clip `clipLength` beats long: count = floor(clipLength / step) (with a 1e-9
// tolerance, so 4 beats of 1/16 is 16), at least 1; swing clamped to 0-1, step to at least 1/64
StepGrid stepGrid(double clipLength, double step, double swing);

// Where step `index` starts: index * step, plus swing * step / 3 on odd steps
double stepStart(const StepGrid &grid, int index);
// The step whose window [index * step, (index + 1) * step) holds `beat` (so a swung note stays in
// its step), or -1 outside [0, count)
int stepAt(const StepGrid &grid, double beat);

// The first note (by start) of `key` starting in step `index`, as an index into `notes`, or -1
int findStep(const std::vector<Note> &notes, int key, const StepGrid &grid, int index);
inline bool stepOn(const std::vector<Note> &notes, int key, const StepGrid &grid, int index)
{
    return findStep(notes, key, grid, index) >= 0;
}

// Turns a step on (adds one note at stepStart, gate steps long, gate clamped to 0.05-1, velocity
// clamped to 0.01-1) or off (removes every note of `key` starting in it). Returns whether `notes`
// changed: turning on a step that is already on, or off one that is off, changes nothing. An
// index outside the grid changes nothing.
bool setStep(std::vector<Note> &notes, int key, const StepGrid &grid, int index, bool on, float velocity, double gate);
// Sets the velocity (clamped to 0.01-1) of every note of `key` starting in the step; false when off
bool setStepVelocity(std::vector<Note> &notes, int key, const StepGrid &grid, int index, float velocity);

// A row as one value a step: the velocity of the step's first note, 0 when off
std::vector<float> rowPattern(const std::vector<Note> &notes, int key, const StepGrid &grid);
// Replaces the row with `pattern` (velocity a step, 0 or less for off; steps past the pattern's
// end are off; extra entries are ignored), each note gate steps long and placed with the swing
void setRow(std::vector<Note> &notes, int key, const StepGrid &grid, const std::vector<float> &pattern, double gate);
// Removes every note of `key` starting inside the grid
void clearRow(std::vector<Note> &notes, int key, const StepGrid &grid);
// Moves the row's notes by `steps` (positive later), wrapping round inside the grid; each note
// keeps its length, velocity, muting and its offset from its step's start, re-swung for its new step
void shiftRow(std::vector<Note> &notes, int key, const StepGrid &grid, int steps);
// Whether the row has notes and every one of them is muted
bool rowMuted(const std::vector<Note> &notes, int key, const StepGrid &grid);
// Mutes or unmutes every note of the row
void setRowMuted(std::vector<Note> &notes, int key, const StepGrid &grid, bool muted);

// Re-places every note starting in an odd step of `grid` for a new swing (0-1), keeping its step
// and length; the notes of every key move, and those in even steps stay where they are
void applySwing(std::vector<Note> &notes, const StepGrid &grid, double swing);

// Patterns, one value a step (velocity, 0 off), for setRow
// Every `every` steps (at least 1) starting at step `offset` (wrapped into 0..every-1)
std::vector<float> everyPattern(int count, int every, int offset, float velocity);
// `pulses` spread as evenly as possible over `steps` (Bjorklund), rotated `rotation` steps later
// (wrapping); pulses clamped to 0..steps. (3, 8, 0) is x..x..x. and (4, 16, 0) is a pulse every 4.
// The first step is a pulse whenever pulses > 0 and rotation is 0.
std::vector<bool> euclidean(int pulses, int steps, int rotation);
// Each step on with probability `density` (0-1), seeded: the same seed gives the same pattern
std::vector<float> randomPattern(int count, double density, float velocity, uint32_t seed);

// Drums: General MIDI's percussion keys
// The rows a new drum clip shows, top to bottom: 36 Kick, 38 Snare, 39 Clap, 42 Closed hat,
// 46 Open hat, 45 Low tom, 50 High tom, 49 Crash
const std::vector<int> &defaultKit();
// The GM percussion name of keys 35-81 ("Kick", "Snare", "Closed hat", ...), else audio::keyName
// ("C#4")
std::string drumName(int key);
// The rows to show, top to bottom: `kit` (in its order, duplicates dropped), then every other key
// the notes use, highest first
std::vector<int> stepRows(const std::vector<Note> &notes, const std::vector<int> &kit);

}
