#include "StepSequence.h"

#include <algorithm>
#include <cmath>
#include <random>

#include "audio/SampleSet.h"

namespace daw {

namespace {

constexpr double kMinStep = 1.0 / 64.0;

// Every mutating function leaves the notes in start order
void sortByStart(std::vector<Note> &notes)
{
    std::stable_sort(notes.begin(), notes.end(), [](const Note &a, const Note &b) { return a.start < b.start; });
}

// Whether a note is of `key` and starts inside the grid's steps (the row's notes)
bool inRow(const Note &n, int key, const StepGrid &grid) { return n.key == key && stepAt(grid, n.start) >= 0; }

double clampGate(double gate) { return std::clamp(gate, 0.05, 1.0); }
float clampStepVelocity(float v) { return std::clamp(v, 0.01f, 1.0f); }

// `index` wrapped into 0..count-1 (count is at least 1)
int wrapIndex(long long index, int count) { return static_cast<int>(((index % count) + count) % count); }

}

const std::vector<StepChoice> &stepChoices()
{
    static const std::vector<StepChoice> all{
        {"1/8", 0.5},
        {"1/16", 0.25},
        {"1/32", 0.125},
        {"1/8T", 1.0 / 3.0},
        {"1/16T", 1.0 / 6.0},
    };
    return all;
}

StepGrid stepGrid(double clipLength, double step, double swing)
{
    StepGrid grid;
    grid.step = std::max(step, kMinStep);
    const double whole = std::floor(clipLength / grid.step + 1e-9);
    grid.count = static_cast<int>(std::clamp(whole, 1.0, 1e7));
    grid.swing = std::clamp(swing, 0.0, 1.0);
    return grid;
}

double stepStart(const StepGrid &grid, int index)
{
    const double start = index * grid.step;
    // Odd steps are pushed late by a third of a step at full swing
    return index % 2 != 0 ? start + grid.swing * grid.step / 3.0 : start;
}

int stepAt(const StepGrid &grid, double beat)
{
    // The unswung window, so a swung note stays in its step
    const double whole = std::floor(beat / grid.step);
    if (!(whole >= 0.0 && whole < grid.count))
        return -1;
    return static_cast<int>(whole);
}

int findStep(const std::vector<Note> &notes, int key, const StepGrid &grid, int index)
{
    if (index < 0 || index >= grid.count)
        return -1;
    int found = -1;
    for (size_t i = 0; i < notes.size(); ++i) {
        const Note &n = notes[i];
        if (n.key != key || stepAt(grid, n.start) != index)
            continue;
        if (found < 0 || n.start < notes[static_cast<size_t>(found)].start)
            found = static_cast<int>(i);
    }
    return found;
}

bool setStep(std::vector<Note> &notes, int key, const StepGrid &grid, int index, bool on, float velocity, double gate)
{
    if (index < 0 || index >= grid.count)
        return false;
    if (on) {
        if (stepOn(notes, key, grid, index))
            return false;
        Note n;
        n.start = stepStart(grid, index);
        n.length = clampGate(gate) * grid.step;
        n.key = key;
        n.velocity = clampStepVelocity(velocity);
        notes.push_back(n);
        sortByStart(notes);
        return true;
    }
    // Removing keeps the order, so nothing needs sorting
    const size_t before = notes.size();
    notes.erase(std::remove_if(notes.begin(), notes.end(),
                               [key, &grid, index](const Note &n) { return n.key == key && stepAt(grid, n.start) == index; }),
                notes.end());
    return notes.size() != before;
}

bool setStepVelocity(std::vector<Note> &notes, int key, const StepGrid &grid, int index, float velocity)
{
    if (index < 0 || index >= grid.count)
        return false;
    bool any = false;
    for (Note &n : notes) {
        if (n.key == key && stepAt(grid, n.start) == index) {
            n.velocity = clampStepVelocity(velocity);
            any = true;
        }
    }
    return any;
}

std::vector<float> rowPattern(const std::vector<Note> &notes, int key, const StepGrid &grid)
{
    const size_t count = static_cast<size_t>(std::max(grid.count, 0));
    std::vector<float> pattern(count, 0.0f);
    // The first note of each step decides its velocity, found in one pass
    std::vector<double> first(count, 0.0);
    std::vector<bool> has(count, false);
    for (const Note &n : notes) {
        if (n.key != key)
            continue;
        const int s = stepAt(grid, n.start);
        if (s < 0)
            continue;
        const size_t i = static_cast<size_t>(s);
        if (!has[i] || n.start < first[i]) {
            has[i] = true;
            first[i] = n.start;
            pattern[i] = n.velocity;
        }
    }
    return pattern;
}

void setRow(std::vector<Note> &notes, int key, const StepGrid &grid, const std::vector<float> &pattern, double gate)
{
    clearRow(notes, key, grid);
    const int steps = std::min(grid.count, static_cast<int>(pattern.size()));
    for (int i = 0; i < steps; ++i) {
        const float velocity = pattern[static_cast<size_t>(i)];
        if (!(velocity > 0.0f))
            continue;
        Note n;
        n.start = stepStart(grid, i);
        n.length = clampGate(gate) * grid.step;
        n.key = key;
        n.velocity = clampStepVelocity(velocity);
        notes.push_back(n);
    }
    sortByStart(notes);
}

void clearRow(std::vector<Note> &notes, int key, const StepGrid &grid)
{
    notes.erase(std::remove_if(notes.begin(), notes.end(), [key, &grid](const Note &n) { return inRow(n, key, grid); }),
                notes.end());
}

void shiftRow(std::vector<Note> &notes, int key, const StepGrid &grid, int steps)
{
    for (Note &n : notes) {
        if (!inRow(n, key, grid))
            continue;
        // Keep the offset from the step's start, and take it to the new step
        const int s = stepAt(grid, n.start);
        const double offset = n.start - stepStart(grid, s);
        const int t = wrapIndex(static_cast<long long>(s) + steps, grid.count);
        n.start = std::max(0.0, stepStart(grid, t) + offset);
    }
    sortByStart(notes);
}

bool rowMuted(const std::vector<Note> &notes, int key, const StepGrid &grid)
{
    bool any = false;
    for (const Note &n : notes) {
        if (!inRow(n, key, grid))
            continue;
        if (!n.muted)
            return false;
        any = true;
    }
    return any;
}

void setRowMuted(std::vector<Note> &notes, int key, const StepGrid &grid, bool muted)
{
    for (Note &n : notes) {
        if (inRow(n, key, grid))
            n.muted = muted;
    }
}

void applySwing(std::vector<Note> &notes, const StepGrid &grid, double swing)
{
    const double next = std::clamp(swing, 0.0, 1.0);
    const double by = (next - grid.swing) * grid.step / 3.0;
    for (Note &n : notes) {
        const int s = stepAt(grid, n.start);
        if (s >= 0 && s % 2 != 0)
            n.start += by;
    }
    sortByStart(notes);
}

std::vector<float> everyPattern(int count, int every, int offset, float velocity)
{
    const size_t size = static_cast<size_t>(std::max(count, 0));
    std::vector<float> pattern(size, 0.0f);
    const int period = std::max(every, 1);
    const int first = static_cast<int>(((offset % period) + period) % period);
    for (int i = first; i < count; i += period)
        pattern[static_cast<size_t>(i)] = clampStepVelocity(velocity);
    return pattern;
}

std::vector<bool> euclidean(int pulses, int steps, int rotation)
{
    std::vector<bool> out;
    if (steps <= 0)
        return out;
    const long long p = std::clamp(pulses, 0, steps);
    const long long n = steps;

    // The even spread: step i is a pulse where floor(i * p / n) steps up, and step 0 always is one
    std::vector<bool> base(static_cast<size_t>(steps), false);
    if (p > 0) {
        for (long long i = 0; i < n; ++i) {
            if (i == 0)
                base[0] = true;
            else
                base[static_cast<size_t>(i)] = (i * p) / n != ((i - 1) * p) / n;
        }
    }

    // Rotated later, wrapping
    const int shift = wrapIndex(rotation, steps);
    out.assign(static_cast<size_t>(steps), false);
    for (int j = 0; j < steps; ++j) {
        if (base[static_cast<size_t>(j)])
            out[static_cast<size_t>((j + shift) % steps)] = true;
    }
    return out;
}

std::vector<float> randomPattern(int count, double density, float velocity, uint32_t seed)
{
    const size_t size = static_cast<size_t>(std::max(count, 0));
    std::vector<float> pattern(size, 0.0f);
    std::mt19937 rng(seed);
    const double chance = std::clamp(density, 0.0, 1.0);
    const float on = clampStepVelocity(velocity);
    for (float &v : pattern) {
        const double roll = static_cast<double>(rng()) / 4294967296.0; // [0, 1)
        if (roll < chance)
            v = on;
    }
    return pattern;
}

const std::vector<int> &defaultKit()
{
    static const std::vector<int> kit{36, 38, 39, 42, 46, 45, 50, 49};
    return kit;
}

std::string drumName(int key)
{
    // General MIDI percussion, keys 35 to 81
    static const char *const names[] = {
        "Kick 2",        "Kick",           "Side stick",     "Snare",          "Clap",          "Snare 2",
        "Low floor tom", "Closed hat",     "High floor tom", "Pedal hat",      "Low tom",       "Open hat",
        "Low mid tom",   "High mid tom",   "Crash",          "High tom",       "Ride",          "China",
        "Ride bell",     "Tambourine",     "Splash",         "Cowbell",        "Crash 2",       "Vibraslap",
        "Ride 2",        "High bongo",     "Low bongo",      "Mute high conga", "Open high conga", "Low conga",
        "High timbale",  "Low timbale",    "High agogo",     "Low agogo",      "Cabasa",        "Maracas",
        "Short whistle", "Long whistle",   "Short guiro",    "Long guiro",     "Claves",        "High wood block",
        "Low wood block", "Mute cuica",    "Open cuica",     "Mute triangle",  "Open triangle",
    };
    if (key >= 35 && key <= 81)
        return names[static_cast<size_t>(key - 35)];
    return audio::keyName(key);
}

std::vector<int> stepRows(const std::vector<Note> &notes, const std::vector<int> &kit)
{
    // The kit first, each key once
    std::vector<int> rows;
    for (int key : kit) {
        if (std::find(rows.begin(), rows.end(), key) == rows.end())
            rows.push_back(key);
    }

    // Then the other keys the notes use, highest first
    std::vector<int> others;
    for (const Note &n : notes) {
        if (std::find(rows.begin(), rows.end(), n.key) == rows.end())
            others.push_back(n.key);
    }
    std::sort(others.begin(), others.end(), [](int a, int b) { return a > b; });
    others.erase(std::unique(others.begin(), others.end()), others.end());
    rows.insert(rows.end(), others.begin(), others.end());
    return rows;
}

}
