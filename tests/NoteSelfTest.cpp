#include "NoteSelfTest.h"
#include "Harness.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "daw/NoteTools.h"
#include "StepSelfTest.h"

namespace tests {

namespace {

using daw::Note;

Note note(double start, double length, int key, float velocity = 0.8f, bool selected = false)
{
    Note n;
    n.start = start;
    n.length = length;
    n.key = key;
    n.velocity = velocity;
    n.selected = selected;
    return n;
}

bool near(double a, double b) { return std::fabs(a - b) < 1e-6; }

std::string keys(const std::vector<Note> &notes)
{
    std::string out;
    for (const Note &n : notes)
        out += (out.empty() ? "" : " ") + std::to_string(n.key);
    return out;
}

std::vector<Note> byStart(std::vector<Note> notes)
{
    std::stable_sort(notes.begin(), notes.end(), [](const Note &a, const Note &b) { return a.start < b.start; });
    return notes;
}

}

int runNoteSelfTest()
{
    Checker c;
    const daw::Scale &major = *daw::findScale("Major");
    const daw::Scale &minorPent = *daw::findScale("Minor pentatonic");

    // Scales
    c.check(daw::inScale(64, 0, major) && !daw::inScale(61, 0, major) && daw::inScale(61, 1, major),
            "Keys in and out of a scale");
    c.check(daw::snapToScale(61, 0, major) == 60 && daw::snapToScale(66, 0, major) == 65 &&
                daw::snapToScale(70, 9, minorPent) == 69,
            "Keys snap to the nearest scale key", std::to_string(daw::snapToScale(66, 0, major)));
    c.check(daw::stepInScale(60, 2, 0, major) == 64 && daw::stepInScale(60, 7, 0, major) == 72 &&
                daw::stepInScale(60, -1, 0, major) == 59 && daw::scaleDegree(67, 0, major) == 4,
            "Steps and degrees within the scale");

    // Chords
    const auto triad = daw::chordKeys(daw::chordShapes()[0], 62, 0, major);
    c.check(triad == std::vector<int>{62, 65, 69}, "A diatonic triad on D in C major is D F A",
            std::to_string(triad.size()) + " keys");
    const auto seventh = daw::chordKeys(daw::chordShapes()[1], 67, 0, major);
    c.check(seventh == std::vector<int>{67, 71, 74, 77}, "A diatonic seventh on G is G B D F");
    const daw::ChordShape *minor = nullptr;
    for (const auto &s : daw::chordShapes()) {
        if (s.name == "Minor")
            minor = &s;
    }
    c.check(minor && daw::chordKeys(*minor, 57, 0, major) == std::vector<int>{57, 60, 64}, "A minor chord on A");

    // Scale runs
    const auto up = daw::scaleRun(60, 1, 1, 2.0, 0.25, 0.7f, 0, major);
    c.check(up.size() == 8 && up.front().key == 60 && up.back().key == 72 && near(up[1].start, 2.25) &&
                near(up[1].length, 0.25) && up[0].selected,
            "A run up the scale, an octave", keys(up));
    const auto updown = daw::scaleRun(60, 1, 0, 0.0, 0.5, 0.7f, 0, major);
    c.check(updown.size() == 15 && updown[7].key == 72 && updown.back().key == 60, "A run up and back down",
            std::to_string(updown.size()) + " notes");
    const auto down = daw::scaleRun(60, 1, -1, 0.0, 0.5, 0.7f, 0, minorPent);
    c.check(down.size() == 6 && down.back().key == 48 && down[1].key == 58, "A run down a pentatonic scale", keys(down));

    // Arpeggiator
    {
        std::vector<Note> notes{note(0, 1, 64), note(0, 1, 60), note(0, 1, 67), note(2, 1, 50)};
        for (int i = 0; i < 3; ++i)
            notes[static_cast<size_t>(i)].selected = true;
        daw::ArpSettings s;
        s.rate = 0.25;
        s.gate = 0.8;
        const int made = daw::arpeggiate(notes, s);
        const auto sorted = byStart(notes);
        c.check(made == 4 && notes.size() == 5 && keys(sorted) == "60 64 67 60 50" && near(sorted[1].start, 0.25) &&
                    near(sorted[1].length, 0.2),
                "Arpeggiate up: a chord becomes a run, others kept", keys(sorted));
    }
    {
        std::vector<Note> notes{note(0, 4, 60), note(0, 4, 64), note(0, 4, 67)};
        daw::ArpSettings s;
        s.pattern = daw::ArpPattern::UpDown;
        s.octaves = 2;
        s.rate = 0.25;
        daw::arpeggiate(notes, s);
        const auto sorted = byStart(notes);
        std::vector<Note> first(sorted.begin(), sorted.begin() + 11);
        c.check(sorted.size() == 16 && keys(first) == "60 64 67 72 76 79 76 72 67 64 60",
                "Arpeggiate up and down over two octaves", keys(first));
    }
    {
        std::vector<Note> notes{note(0, 2, 60), note(0, 2, 64), note(0, 2, 67)};
        daw::ArpSettings s;
        s.pattern = daw::ArpPattern::Random;
        s.rate = 0.125;
        s.seed = 42;
        daw::arpeggiate(notes, s);
        bool inChord = notes.size() == 16;
        for (const Note &n : notes)
            inChord = inChord && (n.key == 60 || n.key == 64 || n.key == 67);
        std::vector<Note> again{note(0, 2, 60), note(0, 2, 64), note(0, 2, 67)};
        daw::arpeggiate(again, s);
        c.check(inChord && keys(byStart(notes)) == keys(byStart(again)), "Random arpeggios stay in the chord and repeat");
    }
    // The newer patterns, the rhythm, ratchets, swing, transposing, octaves, length and velocity
    auto arp = [](std::vector<Note> chord, const daw::ArpSettings &s) {
        daw::arpeggiate(chord, s);
        return byStart(chord);
    };
    const std::vector<Note> cMajor{note(0, 1, 60), note(0, 1, 64), note(0, 1, 67)};
    {
        daw::ArpSettings s;
        s.pattern = daw::ArpPattern::Converge;
        c.check(keys(arp({note(0, 1, 60), note(0, 1, 64), note(0, 1, 67), note(0, 1, 71)}, s)) == "60 71 64 67",
                "Converge: from the outside in");
        s.pattern = daw::ArpPattern::Pinky;
        c.check(keys(arp(cMajor, s)) == "60 67 64 67", "Pinky: the top between the others (Alberti)");
        s.pattern = daw::ArpPattern::Thumb;
        c.check(keys(arp(cMajor, s)) == "60 64 60 67", "Thumb: the bottom between the others");
        s.pattern = daw::ArpPattern::Chord;
        s.rate = 0.5;
        c.check(arp(cMajor, s).size() == 6, "Chord: every key each step");
    }
    {
        daw::ArpSettings s;
        s.rhythm = "x.X-";
        const auto made = arp({note(0, 2, 60), note(0, 2, 64), note(0, 2, 67)}, s);
        c.check(made.size() == 4 && keys(made) == "60 64 67 60" && near(made[1].start, 0.5) && near(made[1].length, 0.45) &&
                    made[1].velocity == 1.0f && made[0].velocity == 0.8f && near(made[3].start, 1.5),
                "Rhythm: rests skip, accents are louder, ties hold", keys(made));
    }
    {
        daw::ArpSettings s;
        s.rate = 0.5;
        s.repeats = 2;
        const auto made = arp({note(0, 1, 60), note(0, 1, 64)}, s);
        c.check(made.size() == 4 && near(made[1].start, 0.25) && near(made[1].length, 0.2) && made[1].key == 60 && made[2].key == 64,
                "Ratchets: each step twice within it");
        daw::ArpSettings w;
        w.rate = 0.5;
        w.swing = 1.0;
        const auto swung = arp({note(0, 1, 60), note(0, 1, 64)}, w);
        c.check(swung.size() == 2 && near(swung[1].start, 0.5 + 0.5 / 3.0), "Swing delays every second step");
        daw::ArpSettings t;
        t.transpose = 12;
        c.check(keys(arp({note(0, 1, 60), note(0, 1, 64)}, t)) == "60 64 72 76", "Transpose each time round");
        daw::ArpSettings o;
        o.octaves = 2;
        o.octaveMode = daw::ArpOctaves::Down;
        o.rate = 0.5;
        c.check(keys(arp({note(0, 1, 60)}, o)) == "48 60", "Octaves stacked downwards");
        daw::ArpSettings l;
        l.rate = 0.5;
        l.length = 2.0;
        c.check(arp({note(0, 1, 60)}, l).size() == 4, "A set length outlasts the chord");
        daw::ArpSettings v;
        v.rate = 0.5;
        v.velocityMode = daw::ArpVelocity::Rising;
        v.velocity = 1.0f;
        const auto rising = arp({note(0, 1.5, 60)}, v);
        c.check(rising.size() == 3 && near(rising[0].velocity, 0.35) && near(rising[2].velocity, 1.0), "Velocity rises across the chord");
        bool presets = !daw::arpPresets().empty();
        for (const daw::ArpPreset &p : daw::arpPresets())
            presets = presets && !arp(cMajor, p.settings).empty();
        c.check(presets, "Every preset makes notes");
    }

    // Quantize, legato
    {
        std::vector<Note> notes{note(0.13, 0.4, 60), note(1.1, 0.3, 62)};
        daw::quantize(notes, 0.25);
        c.check(near(notes[0].start, 0.25) && near(notes[0].length, 0.4) && near(notes[1].start, 1.0),
                "Quantize moves starts to the grid, keeping lengths");
        std::vector<Note> half{note(0.1, 0.4, 60)};
        daw::quantize(half, 0.5, 0.5);
        c.check(near(half[0].start, 0.05), "Quantize halfway");
        std::vector<Note> ends{note(0.1, 0.4, 60)};
        daw::quantize(ends, 0.25, 1.0, true);
        c.check(near(ends[0].start, 0.0) && near(ends[0].length, 0.5), "Quantize starts and ends");
        std::vector<Note> sel{note(0.13, 0.4, 60, 0.8f, true), note(1.1, 0.3, 62)};
        daw::quantize(sel, 0.25);
        c.check(near(sel[1].start, 1.1), "Tools act on the selection when there is one");
    }
    {
        std::vector<Note> notes{note(0, 0.1, 60), note(0, 0.1, 64), note(1, 0.1, 62), note(3, 0.5, 65)};
        daw::legato(notes);
        c.check(near(notes[0].length, 1.0) && near(notes[1].length, 1.0) && near(notes[2].length, 2.0) &&
                    near(notes[3].length, 0.5),
                "Legato: each lasts until the next start");
    }

    // Velocity
    {
        std::vector<Note> notes{note(0, 1, 60, 0.5f), note(1, 1, 60, 0.5f), note(2, 1, 60, 0.5f)};
        daw::rampVelocity(notes, 0.2f, 1.0f);
        c.check(std::fabs(notes[0].velocity - 0.2f) < 1e-5f && std::fabs(notes[1].velocity - 0.6f) < 1e-5f &&
                    std::fabs(notes[2].velocity - 1.0f) < 1e-5f,
                "A velocity ramp across the notes in time");
        daw::scaleVelocity(notes, 0.5f);
        c.check(std::fabs(notes[0].velocity - 0.4f) < 1e-5f && std::fabs(notes[2].velocity - 0.8f) < 1e-5f,
                "Compressing velocities around their middle");
        daw::shiftVelocity(notes, 0.5f);
        c.check(notes[2].velocity == 1.0f && std::fabs(notes[0].velocity - 0.9f) < 1e-5f, "Louder, clamped at 127");
        daw::setVelocity(notes, -1.0f);
        c.check(notes[0].velocity > 0.0f && notes[0].velocity < 0.01f, "Velocities stay at least 1");
    }

    // Duplicate, reverse, invert, chop, join, overlaps
    {
        std::vector<Note> notes{note(0, 0.5, 60, 0.8f, true), note(0.5, 0.5, 62, 0.8f, true), note(4, 1, 70)};
        const int copies = daw::duplicate(notes, 0.5);
        c.check(copies == 2 && notes.size() == 5 && near(notes[3].start, 1.0) && near(notes[4].start, 1.5) &&
                    notes[3].selected && !notes[0].selected,
                "Duplicate places copies after the selection");
    }
    {
        std::vector<Note> notes{note(0, 1, 60), note(1, 2, 64)};
        daw::reverse(notes);
        c.check(near(notes[0].start, 2.0) && near(notes[1].start, 0.0), "Reverse plays them backwards");
        daw::invert(notes);
        c.check(notes[0].key == 64 && notes[1].key == 60, "Invert turns them upside down");
    }
    {
        std::vector<Note> notes{note(0, 1, 60)};
        daw::chop(notes, 0.25);
        c.check(notes.size() == 4 && near(notes[3].start, 0.75) && near(notes[3].length, 0.25), "Chop to the grid");
        daw::join(notes);
        c.check(notes.size() == 1 && near(notes[0].length, 1.0), "Join puts them back together");
        std::vector<Note> overlap{note(0, 2, 60), note(1, 1, 60), note(0, 3, 62)};
        daw::fixOverlaps(overlap);
        c.check(near(overlap[0].length, 1.0) && near(overlap[2].length, 3.0), "Fix overlaps on the same key");
    }

    // Copy and paste, clip bounds
    {
        std::vector<Note> notes{note(2, 1, 60, 0.8f, true), note(3, 1, 62, 0.8f, true)};
        const auto clipboard = daw::copySelection(notes);
        daw::paste(notes, clipboard, 6.0);
        c.check(notes.size() == 4 && near(notes[2].start, 6.0) && near(notes[3].start, 7.0) && notes[3].selected &&
                    !notes[0].selected,
                "Paste puts copies at the point given");
        daw::clampToClip(notes, 6.5, 1.0 / 64.0);
        c.check(notes.size() == 3 && near(notes[2].length, 0.5), "Notes past the clip's end are dropped",
                std::to_string(notes.size()) + " left");
        std::vector<Note> longer{note(7, 4, 60)};
        daw::clampToClip(longer, 8.0, 1.0 / 64.0);
        c.check(near(longer[0].length, 1.0), "Notes are cut at the clip's end");
    }

    // Muting, nudging, resizing, stretching, slicing
    {
        std::vector<Note> notes{note(0, 1, 60, 0.8f, true), note(1, 1, 62, 0.8f, true), note(2, 1, 64)};
        notes[1].muted = true;
        const int muted = daw::toggleMute(notes);
        c.check(muted == 1 && notes[0].muted && notes[1].muted && !notes[2].muted, "Mute: any playing mutes them all");
        daw::toggleMute(notes);
        c.check(!notes[0].muted && !notes[1].muted && !notes[2].muted, "Mute again: they all play");
        daw::nudge(notes, -0.5);
        c.check(near(notes[0].start, 0.0) && near(notes[1].start, 1.0) && near(notes[2].start, 2.0),
                "Nudge stops at the start and keeps the spacing");
        daw::nudge(notes, 0.125);
        c.check(near(notes[0].start, 0.125) && near(notes[1].start, 1.125) && near(notes[2].start, 2.0), "Nudge later");
        daw::resizeBy(notes, -2.0, 1.0 / 64.0);
        c.check(near(notes[0].length, 1.0 / 64.0) && near(notes[2].length, 1.0), "Resize keeps a minimum length");
        std::vector<Note> stretched{note(1, 1, 60, 0.8f, true), note(2, 1, 62, 0.8f, true)};
        daw::stretchAround(stretched, 3.0, 2.0, 1.0 / 64.0);
        c.check(near(stretched[0].start, 0.0) && near(stretched[1].start, 1.0) && near(stretched[1].length, 2.0),
                "Stretch around the selection's end");
        std::vector<Note> sliced{note(0, 2, 60, 0.6f)};
        const bool cut = daw::slice(sliced, 0, 0.5, 1.0 / 64.0);
        c.check(cut && sliced.size() == 2 && near(sliced[0].length, 0.5) && near(sliced[1].start, 0.5) &&
                    near(sliced[1].length, 1.5) && sliced[1].velocity == 0.6f,
                "Slice cuts a note in two");
        c.check(!daw::slice(sliced, 0, 0.5, 1.0 / 64.0) && sliced.size() == 2, "Slicing at a note's edge does nothing");
    }

    // Selection: lasso, same keys, next and previous, bounds
    {
        std::vector<Note> notes{note(0, 1, 60), note(2, 1, 64), note(4, 1, 60), note(1, 0.5, 72)};
        // A triangle over beats 0-3 and keys 58-66: the first two notes, not the high one
        const std::vector<daw::NotePoint> lasso{{-0.5, 58.0}, {3.5, 58.0}, {1.5, 67.0}};
        c.check(daw::insidePolygon(lasso, {1.5, 60.0}) && !daw::insidePolygon(lasso, {1.5, 72.0}), "A point in a lasso");
        const int lassoed = daw::selectInLasso(notes, lasso, false);
        c.check(lassoed == 2 && notes[0].selected && notes[1].selected && !notes[2].selected && !notes[3].selected,
                "Lasso selects the notes it encloses", std::to_string(lassoed));
        notes[1].selected = false;
        c.check(daw::selectSameKeys(notes) == 1 && notes[2].selected, "Select the same keys");
        daw::selectAll(notes, false);
        notes[0].selected = true;
        c.check(daw::selectNext(notes, 1) == 3 && notes[3].selected && !notes[0].selected, "Next note in time");
        c.check(daw::selectNext(notes, -1) == 0 && notes[0].selected, "Previous note in time");
        notes[1].selected = true;
        const daw::NoteBounds b = daw::noteBounds(notes);
        c.check(b.count == 2 && near(b.start, 0.0) && near(b.end, 3.0) && b.low == 60 && b.high == 64, "Selection bounds");
    }

    // Controller lanes: points, lines, erasing, thinning, values between points
    {
        std::vector<daw::ControlLane> lanes;
        c.check(daw::findLane(lanes, 1) == nullptr, "No lane until one is made");
        daw::ControlLane &mod = daw::laneFor(lanes, 1);
        daw::setControlPoint(mod, 2.0, 1.0f, 0.0);
        daw::setControlPoint(mod, 0.0, 0.0f, 0.0);
        daw::setControlPoint(mod, 2.01, 0.5f, 0.05); // replaces the one at 2
        c.check(mod.points.size() == 2 && near(mod.points[1].beat, 2.01) && mod.points[1].value == 0.5f,
                "Points stay sorted, a near one replaced");
        c.check(near(mod.valueAt(1.005), 0.25) && near(mod.valueAt(-1.0), 0.0) && near(mod.valueAt(9.0), 0.5),
                "Values between points are on the line", std::to_string(mod.valueAt(1.005)));
        daw::ControlLane &pedal = daw::laneFor(lanes, 64);
        daw::setControlPoint(pedal, 0.0, 1.0f, 0.0);
        daw::setControlPoint(pedal, 4.0, 0.0f, 0.0);
        c.check(pedal.valueAt(3.9) == 1.0f && pedal.valueAt(4.0) == 0.0f, "The sustain pedal holds until its next point");
        daw::ControlLane &bend = daw::laneFor(lanes, daw::kPitchBend);
        c.check(lanes.size() == 3 && bend.valueAt(1.0) == 0.5f, "An empty bend rests in the middle");
        daw::ControlLane &ramp = daw::laneFor(lanes, 11);
        for (int i = 0; i <= 16; ++i)
            daw::setControlPoint(ramp, i * 0.25, static_cast<float>(i) / 16.0f, 0.0);
        const int thinned = daw::thinControlPoints(ramp, 0.01f);
        c.check(ramp.points.size() == 2 && thinned == 15, "Thinning a painted ramp leaves its ends", std::to_string(ramp.points.size()));
        daw::controlLine(ramp, 1.0, 1.0f, 3.0, 0.0f);
        c.check(ramp.points.size() == 4 && near(ramp.valueAt(2.0), 0.5), "A line replaces the points it spans");
        c.check(daw::eraseControlPoints(ramp, 0.5, 3.5) == 2 && ramp.points.size() == 2, "Erasing a range");
        bend.points.clear();
        daw::dropEmptyLanes(lanes);
        c.check(lanes.size() == 3 && daw::findLane(lanes, daw::kPitchBend) == nullptr, "Empty lanes go");
        c.check(daw::controllerName(1) == "Mod wheel" && daw::controllerName(daw::kPitchBend) == "Pitch bend" &&
                    daw::controllerName(20) == "CC 20",
                "Controller names");
    }

    // The automatic grid and grid names
    {
        c.check(near(daw::autoGrid(48.0, 12.0, false), 0.25), "Auto grid at 48 pixels a beat is 1/16",
                std::to_string(daw::autoGrid(48.0, 12.0, false)));
        c.check(near(daw::autoGrid(400.0, 12.0, false), 1.0 / 32.0) && near(daw::autoGrid(2.0, 12.0, false), 4.0),
                "Auto grid follows the zoom");
        c.check(near(daw::autoGrid(48.0, 12.0, true), 1.0 / 3.0), "Auto grid in triplets");
        c.check(daw::gridName(0.25, 4) == "1/16" && daw::gridName(1.0 / 3.0, 4) == "1/8T" && daw::gridName(4.0, 4) == "1 bar" &&
                    daw::gridName(1.0, 3) == "1/4",
                "Grid names", daw::gridName(1.0 / 3.0, 4));
    }

    c.failures += runStepChecks();

    return c.finish("notes");
}

}
