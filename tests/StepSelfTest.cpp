#include "StepSelfTest.h"
#include "Harness.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "daw/model/StepSequence.h"

namespace tests {

namespace {

using daw::Note;
using daw::StepGrid;

Note note(double start, double length, int key, float velocity = 0.8f, bool muted = false)
{
    Note n;
    n.start = start;
    n.length = length;
    n.key = key;
    n.velocity = velocity;
    n.muted = muted;
    return n;
}

bool near(double a, double b) { return std::fabs(a - b) < 1e-6; }
bool nearF(float a, float b) { return std::fabs(a - b) < 1e-5f; }

bool sorted(const std::vector<Note> &notes)
{
    for (size_t i = 1; i < notes.size(); ++i) {
        if (notes[i].start < notes[i - 1].start)
            return false;
    }
    return true;
}

int countKey(const std::vector<Note> &notes, int key)
{
    int n = 0;
    for (const Note &x : notes) {
        if (x.key == key)
            ++n;
    }
    return n;
}

const Note *first(const std::vector<Note> &notes, int key)
{
    for (const Note &x : notes) {
        if (x.key == key)
            return &x;
    }
    return nullptr;
}

bool hasNote(const std::vector<Note> &notes, int key, double start)
{
    for (const Note &x : notes) {
        if (x.key == key && near(x.start, start))
            return true;
    }
    return false;
}

int countTrue(const std::vector<bool> &v)
{
    int n = 0;
    for (bool b : v) {
        if (b)
            ++n;
    }
    return n;
}

// x for a pulse, . for a rest
std::string boolText(const std::vector<bool> &v)
{
    std::string s;
    for (bool b : v)
        s += b ? 'x' : '.';
    return s;
}

std::string levelText(const std::vector<float> &v)
{
    std::string s;
    for (float f : v)
        s += f > 0.0f ? 'x' : '.';
    return s;
}

// The text of `steps` steps with a pulse at every `every`th step starting at `offset`
std::string wantText(int steps, int every, int offset)
{
    std::string s;
    for (int i = 0; i < steps; ++i)
        s += (i % every == offset) ? 'x' : '.';
    return s;
}

std::string intsText(const std::vector<int> &v)
{
    std::string s;
    for (int x : v)
        s += (s.empty() ? "" : " ") + std::to_string(x);
    return s;
}

}

int runStepChecks()
{
    Checker c;
    const StepGrid g = daw::stepGrid(4.0, 0.25, 0.0);
    const StepGrid swung = daw::stepGrid(4.0, 0.25, 0.5);
    const double oddShift = 0.5 * 0.25 / 3.0; // swing 0.5 on a 1/16 step
    const auto &choices = daw::stepChoices();

    // Step sizes and the grid
    c.check(choices.size() == 5 && std::string(choices[0].name) == "1/8" && std::string(choices[1].name) == "1/16" &&
                std::string(choices[2].name) == "1/32" && std::string(choices[3].name) == "1/8T" &&
                std::string(choices[4].name) == "1/16T",
            "Step sizes are 1/8, 1/16, 1/32, 1/8T and 1/16T");
    c.check(near(choices[0].beats, 0.5) && near(choices[1].beats, 0.25) && near(choices[2].beats, 0.125) &&
                near(choices[3].beats, 1.0 / 3.0) && near(choices[4].beats, 1.0 / 6.0),
            "Step sizes are half, quarter, eighth, third and sixth of a beat");
    c.check(std::string(choices[daw::kDefaultStepChoice].name) == "1/16", "The default step is 1/16");
    c.check(g.count == 16 && near(g.step, 0.25) && near(g.swing, 0.0), "4 beats of 1/16 is 16 steps", std::to_string(g.count));
    c.check(daw::stepGrid(4.0, choices[3].beats, 0.0).count == 12, "4 beats of 1/8T is 12 steps");
    c.check(daw::stepGrid(0.1, 0.25, 0.0).count == 1 && daw::stepGrid(0.0, 0.25, 0.0).count == 1,
            "A tiny clip still has one step");
    c.check(near(daw::stepGrid(4.0, 0.25, 2.0).swing, 1.0) && near(daw::stepGrid(4.0, 0.25, -0.5).swing, 0.0),
            "Swing is clamped to 0-1");
    c.check(near(daw::stepGrid(4.0, 0.0, 0.0).step, 1.0 / 64.0), "A step is at least 1/64 of a beat");

    // Where steps start, and which step a beat is in
    c.check(near(daw::stepStart(g, 0), 0.0) && near(daw::stepStart(g, 2), 0.5) && near(daw::stepStart(g, 3), 0.75),
            "Without swing a step starts at its index times the step");
    c.check(near(daw::stepStart(swung, 2), 0.5), "Even steps stay put under swing");
    c.check(near(daw::stepStart(swung, 1), 0.25 + oddShift) && near(daw::stepStart(swung, 3), 0.75 + oddShift),
            "Odd steps start a third of the swing late", std::to_string(daw::stepStart(swung, 1)));
    c.check(daw::stepAt(g, 0.0) == 0 && daw::stepAt(g, 0.25) == 1 && daw::stepAt(g, 3.99) == 15,
            "A beat is in the step whose window holds it");
    c.check(daw::stepAt(swung, daw::stepStart(swung, 1)) == 1 && daw::stepAt(swung, daw::stepStart(swung, 1) + 0.1) == 1,
            "A swung note stays in its step");
    c.check(daw::stepAt(g, -0.1) == -1 && daw::stepAt(g, 4.0) == -1 && daw::stepAt(g, 100.0) == -1,
            "A beat outside the grid has no step");

    // Finding steps
    const std::vector<Note> row{note(0.0, 1, 60), note(0.25, 1, 60), note(0.3, 1, 62), note(0.5, 1, 60)};
    c.check(daw::findStep(row, 60, g, 0) == 0 && daw::findStep(row, 60, g, 1) == 1 && daw::findStep(row, 60, g, 2) == 3 &&
                daw::findStep(row, 60, g, 3) == -1,
            "findStep gives the note of a key in a step, else -1");
    c.check(daw::findStep(row, 62, g, 1) == 2 && !daw::stepOn(row, 64, g, 1) && daw::stepOn(row, 60, g, 2),
            "stepOn follows findStep for each key");
    const std::vector<Note> swungNote{note(daw::stepStart(swung, 1), 0.1, 60)};
    c.check(daw::findStep(swungNote, 60, swung, 1) == 0 && daw::findStep(swungNote, 60, swung, 0) == -1,
            "A swung note is found in its own step");

    // Turning steps on and off
    std::vector<Note> notes;
    const bool added = daw::setStep(notes, 60, g, 2, true, 0.6f, 0.5);
    c.check(added && notes.size() == 1 && near(notes[0].start, 0.5) && near(notes[0].length, 0.125) &&
                nearF(notes[0].velocity, 0.6f) && notes[0].key == 60 && !notes[0].selected,
            "A step on adds a note at its start, gate steps long, velocity as given, unselected");
    std::vector<Note> clamp;
    daw::setStep(clamp, 60, g, 3, true, 2.0f, 2.0);
    daw::setStep(clamp, 60, g, 4, true, -1.0f, 0.0);
    c.check(clamp.size() == 2 && nearF(clamp[0].velocity, 1.0f) && near(clamp[0].length, 0.25) &&
                nearF(clamp[1].velocity, 0.01f) && near(clamp[1].length, 0.0125),
            "Velocity clamped to 0.01-1 and gate to 0.05-1");
    c.check(!daw::setStep(notes, 60, g, 2, true, 0.6f, 0.5) && notes.size() == 1,
            "Turning on a step that is on changes nothing");
    c.check(daw::setStep(notes, 60, g, 2, false, 0.6f, 0.5) && notes.empty(), "A step off removes its note");
    c.check(!daw::setStep(notes, 60, g, 2, false, 0.6f, 0.5) && notes.empty(),
            "Turning off a step that is off changes nothing");
    std::vector<Note> two{note(0.5, 1, 60), note(0.55, 1, 62), note(0.6, 1, 60)};
    c.check(daw::setStep(two, 60, g, 2, false, 0.6f, 0.5) && two.size() == 1 && two[0].key == 62,
            "A step off removes every note of its key starting in it");
    std::vector<Note> edge{note(0.0, 1, 60)};
    const bool outHigh = daw::setStep(edge, 60, g, 16, true, 0.8f, 0.5);
    const bool outLow = daw::setStep(edge, 60, g, -1, true, 0.8f, 0.5);
    c.check(!outHigh && !outLow && edge.size() == 1 && near(edge[0].start, 0.0),
            "A step index outside the grid changes nothing");
    std::vector<Note> others{note(0.0, 2, 36, 0.9f), note(1.0, 1, 62, 0.4f)};
    daw::setStep(others, 60, g, 1, true, 0.8f, 0.5);
    const Note *kick = first(others, 36);
    const Note *high = first(others, 62);
    c.check(others.size() == 3 && kick && near(kick->length, 2.0) && nearF(kick->velocity, 0.9f) && high &&
                near(high->start, 1.0) && nearF(high->velocity, 0.4f),
            "Notes of other keys are left as they were");
    c.check(sorted(others) && hasNote(others, 60, 0.25), "A step added lands in start order");
    std::vector<Note> sw;
    daw::setStep(sw, 60, swung, 1, true, 0.8f, 0.5);
    c.check(sw.size() == 1 && near(sw[0].start, daw::stepStart(swung, 1)) && daw::stepOn(sw, 60, swung, 1),
            "A step on a swung grid starts at its swung point");

    // Velocity of a step's notes
    std::vector<Note> vel{note(0.25, 1, 60, 0.8f), note(0.3, 1, 60, 0.8f)};
    const bool v1 = daw::setStepVelocity(vel, 60, g, 1, 0.3f);
    const bool v2 = daw::setStepVelocity(vel, 60, g, 1, 5.0f);
    const bool v3 = daw::setStepVelocity(vel, 60, g, 2, 0.5f);
    c.check(v1 && v2 && !v3 && nearF(vel[0].velocity, 1.0f) && nearF(vel[1].velocity, 1.0f),
            "Velocity of every note in a step set and clamped; false when off");

    // Rows as patterns
    const std::vector<Note> pat{note(0.0, 1, 60, 0.5f), note(1.0, 1, 60, 0.9f), note(0.25, 1, 62, 0.7f)};
    const auto p = daw::rowPattern(pat, 60, g);
    c.check(p.size() == 16 && nearF(p[0], 0.5f) && nearF(p[4], 0.9f) && p[1] == 0.0f && p[2] == 0.0f,
            "A row reads as its velocities, 0 for off, other keys ignored");
    const std::vector<Note> twin{note(0.5, 1, 60, 0.4f), note(0.6, 1, 60, 0.9f)};
    c.check(nearF(daw::rowPattern(twin, 60, g)[2], 0.4f), "A step's velocity is its first note's");

    // Replacing a row
    std::vector<Note> row2{note(0.25, 1, 60, 0.8f), note(0.5, 1, 62, 0.6f), note(3.5, 1, 60, 0.8f), note(4.5, 1, 60, 0.8f)};
    daw::setRow(row2, 60, g, {0.5f, 0.0f, 0.8f, 0.0f}, 0.5);
    const auto after = daw::rowPattern(row2, 60, g);
    c.check(after.size() == 16 && nearF(after[0], 0.5f) && after[1] == 0.0f && nearF(after[2], 0.8f),
            "setRow replaces the row inside the grid", levelText(after));
    c.check(after[14] == 0.0f && after[3] == 0.0f, "A shorter pattern leaves the rest of the row off");
    c.check(hasNote(row2, 60, 4.5), "setRow keeps notes of the key past the grid");
    c.check(countKey(row2, 62) == 1 && hasNote(row2, 62, 0.5), "setRow leaves other keys alone");
    c.check(sorted(row2), "setRow leaves notes in start order");
    std::vector<Note> gated;
    daw::setRow(gated, 60, swung, {0.0f, 0.5f}, 0.5);
    c.check(gated.size() == 1 && near(gated[0].start, daw::stepStart(swung, 1)) && near(gated[0].length, 0.125) &&
                nearF(gated[0].velocity, 0.5f),
            "setRow places notes swung, gate steps long");
    const StepGrid oneBeat = daw::stepGrid(1.0, 0.25, 0.0);
    std::vector<Note> extra;
    daw::setRow(extra, 60, oneBeat, {0.5f, 0.5f, 0.5f, 0.5f, 0.9f, 0.9f}, 0.5);
    c.check(extra.size() == 4, "Pattern entries past the grid are ignored", std::to_string(extra.size()));
    std::vector<Note> neg;
    daw::setRow(neg, 60, g, {-1.0f, 0.0f, 0.3f}, 1.0);
    c.check(neg.size() == 1 && near(neg[0].start, 0.5) && near(neg[0].length, 0.25) && nearF(neg[0].velocity, 0.3f),
            "Velocities of 0 or less are off");

    // Clearing a row
    std::vector<Note> cl{note(0.25, 1, 60), note(0.5, 1, 62), note(3.9, 1, 60), note(4.5, 1, 60)};
    daw::clearRow(cl, 60, g);
    c.check(cl.size() == 2 && countKey(cl, 60) == 1 && hasNote(cl, 60, 4.5) && countKey(cl, 62) == 1,
            "clearRow removes the row's notes in the grid only");

    // Shifting a row
    std::vector<Note> sh{note(0.0, 0.5, 60, 0.3f, true), note(1.0, 1, 62)};
    daw::shiftRow(sh, 60, g, 1);
    const Note *m = first(sh, 60);
    c.check(m && near(m->start, 0.25) && near(m->length, 0.5) && nearF(m->velocity, 0.3f) && m->muted &&
                hasNote(sh, 62, 1.0) && sorted(sh),
            "shiftRow moves a row a step, keeping length, velocity and mute");
    std::vector<Note> back{note(0.0, 1, 60)};
    daw::shiftRow(back, 60, g, -1);
    c.check(back.size() == 1 && near(back[0].start, 3.75), "Shifting earlier from the start wraps to the end");
    std::vector<Note> fwd{note(3.75, 1, 60)};
    daw::shiftRow(fwd, 60, g, 1);
    c.check(fwd.size() == 1 && near(fwd[0].start, 0.0), "Shifting later from the end wraps to the start");
    std::vector<Note> shifted{note(0.1, 1, 60)};
    daw::shiftRow(shifted, 60, g, 1);
    c.check(shifted.size() == 1 && near(shifted[0].start, 0.35), "A shifted note keeps its offset from its step");
    std::vector<Note> reswung{note(0.0, 1, 60)};
    daw::shiftRow(reswung, 60, swung, 1);
    c.check(reswung.size() == 1 && near(reswung[0].start, daw::stepStart(swung, 1)),
            "A shifted note is swung for its new step");

    // Muting a row
    const std::vector<Note> none;
    c.check(!daw::rowMuted(none, 60, g), "An empty row is not muted");
    std::vector<Note> mutes{note(0.0, 1, 60, 0.8f, true), note(1.0, 1, 60, 0.8f, true)};
    const bool allMuted = daw::rowMuted(mutes, 60, g);
    mutes.push_back(note(2.0, 1, 60));
    c.check(allMuted && !daw::rowMuted(mutes, 60, g), "A row is muted only when every note of it is");
    daw::setRowMuted(mutes, 60, g, true);
    const bool nowMuted = daw::rowMuted(mutes, 60, g);
    std::vector<Note> mixed{note(0.0, 1, 60), note(0.5, 1, 62)};
    daw::setRowMuted(mixed, 60, g, true);
    c.check(nowMuted && mixed[0].muted && !mixed[1].muted, "setRowMuted mutes that row's notes only");
    daw::setRowMuted(mutes, 60, g, false);
    c.check(!daw::rowMuted(mutes, 60, g), "setRowMuted false plays the row again");

    // Swing over a whole clip
    std::vector<Note> sws{note(0.0, 1, 60), note(0.25, 1, 60), note(0.5, 1, 62), note(0.75, 1, 36)};
    daw::applySwing(sws, g, 0.5);
    c.check(hasNote(sws, 60, 0.0) && hasNote(sws, 60, daw::stepStart(swung, 1)) && hasNote(sws, 62, 0.5) &&
                hasNote(sws, 36, daw::stepStart(swung, 3)),
            "applySwing moves odd-step notes of every key only");
    daw::applySwing(sws, swung, 0.0); // the grid carries the swing the notes have now
    c.check(hasNote(sws, 60, 0.25) && hasNote(sws, 36, 0.75), "Swing back to 0 restores the original starts");

    // Patterns
    c.check(levelText(daw::everyPattern(16, 4, 0, 0.7f)) == wantText(16, 4, 0) &&
                nearF(daw::everyPattern(16, 4, 0, 0.7f)[4], 0.7f),
            "everyPattern(16, 4, 0) pulses every fourth step from the start");
    c.check(levelText(daw::everyPattern(16, 4, 1, 0.7f)) == wantText(16, 4, 1), "everyPattern with an offset starts there");
    c.check(levelText(daw::everyPattern(16, 4, 5, 0.7f)) == wantText(16, 4, 1), "An offset past the period wraps into it");
    c.check(levelText(daw::everyPattern(4, 0, 0, 0.5f)) == "xxxx", "Every below 1 means every step");
    c.check(boolText(daw::euclidean(3, 8, 0)) == "x..x..x.", "euclidean(3, 8) is x..x..x.", boolText(daw::euclidean(3, 8, 0)));
    c.check(boolText(daw::euclidean(4, 16, 0)) == wantText(16, 4, 0), "euclidean(4, 16) is a pulse every 4 steps");
    const auto five = daw::euclidean(5, 8, 0);
    c.check(five.size() == 8 && countTrue(five) == 5 && five[0], "euclidean(5, 8) has 5 pulses, the first on");
    c.check(boolText(daw::euclidean(3, 8, 1)) == ".x..x..x", "A rotation moves the pulses later", boolText(daw::euclidean(3, 8, 1)));
    c.check(countTrue(daw::euclidean(0, 8, 0)) == 0 && countTrue(daw::euclidean(20, 8, 0)) == 8,
            "No pulses is silence, and more pulses than steps fills them");
    const auto r1 = daw::randomPattern(32, 0.5, 0.6f, 7);
    const auto r2 = daw::randomPattern(32, 0.5, 0.6f, 7);
    c.check(r1.size() == 32 && r1 == r2, "The same seed gives the same random pattern");
    const auto empty = daw::randomPattern(32, 0.0, 0.6f, 7);
    const auto full = daw::randomPattern(32, 1.0, 0.6f, 7);
    bool emptyOk = empty.size() == 32;
    bool fullOk = full.size() == 32;
    for (float f : empty)
        emptyOk = emptyOk && f == 0.0f;
    for (float f : full)
        fullOk = fullOk && nearF(f, 0.6f);
    c.check(emptyOk && fullOk, "Density 0 is an empty pattern and density 1 a full one");

    // Drums
    c.check(daw::defaultKit() == std::vector<int>{36, 38, 39, 42, 46, 45, 50, 49},
            "The default kit is the eight drums in order", intsText(daw::defaultKit()));
    c.check(daw::drumName(36) == "Kick" && daw::drumName(42) == "Closed hat", "Kick and closed hat are named");
    const std::string gm = daw::drumName(60);
    c.check(!gm.empty() && gm != "C4", "Key 60 has its General MIDI name", gm);
    c.check(daw::drumName(30) == "F#1", "Keys outside 35-81 are named by their key", daw::drumName(30));
    c.check(daw::stepRows({}, daw::defaultKit()) == daw::defaultKit(), "With no notes the kit's rows are shown");
    const std::vector<Note> drumNotes{note(0, 1, 64), note(0, 1, 36), note(1, 1, 70), note(2, 1, 70), note(3, 1, 38), note(4, 1, 64)};
    const auto rows = daw::stepRows(drumNotes, {36, 38, 36, 39});
    c.check(rows == std::vector<int>{36, 38, 39, 70, 64}, "Rows: the kit first, other keys highest first, no repeats",
            intsText(rows));

    return c.finish("steps");
}

}
