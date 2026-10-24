#include "ExpressionSelfTest.h"
#include "Harness.h"

#include <cmath>
#include <string>
#include <vector>

#include "daw/io/ProjectFile.h"
#include "daw/model/History.h"
#include "daw/model/NoteTools.h"

namespace tests {

namespace {

using daw::ControlPoint;
using daw::Expression;
using daw::Note;

constexpr Expression kTuning = Expression::Tuning;

Note tuned(double start, double length, int key, std::vector<ControlPoint> points, bool selected = true)
{
    Note n;
    n.start = start;
    n.length = length;
    n.key = key;
    n.selected = selected;
    n.setCurve(kTuning, std::move(points));
    return n;
}

bool nearF(float a, float b) { return std::fabs(a - b) < 1e-4f; }

std::string pointsText(const Note &n)
{
    const daw::NoteCurve *curve = n.curve(kTuning);
    if (!curve)
        return "none";
    std::string out;
    for (const ControlPoint &p : curve->points)
        out += (out.empty() ? "" : " ") + std::to_string(p.beat) + ":" + std::to_string(p.value);
    return out;
}

// The first note of the first instrument clip with notes
Note *firstNote(daw::Project &p)
{
    for (daw::Track &t : p.tracks) {
        for (daw::Clip &c : t.clips) {
            if (t.kind == daw::TrackKind::Instrument && !c.notes.empty())
                return &c.notes.front();
        }
    }
    return nullptr;
}

}

int runExpressionChecks()
{
    Checker c;

    // A curve's values
    {
        const Note n = tuned(0, 2, 60, {{0.5, 0.0f}, {1.5, 2.0f}});
        c.check(nearF(n.expressionAt(kTuning, 0.0), 0.0f) && nearF(n.expressionAt(kTuning, 1.0), 1.0f) &&
                    nearF(n.expressionAt(kTuning, 9.0), 2.0f),
                "Expression: a curve holds before its first point, joins its points, holds after its last");
        Note plain;
        c.check(plain.curve(kTuning) == nullptr && nearF(plain.expressionAt(kTuning, 0.5), 0.0f),
                "Expression: a note without a curve is at the neutral value");
        Note cleared = n;
        cleared.setCurve(kTuning, {});
        c.check(cleared.expression.empty(), "Expression: setting no points removes the curve");
        daw::Expression read = kTuning;
        c.check(std::string(daw::expressionKey(kTuning)) == "tuning" && daw::expressionFromKey("tuning", read) &&
                    !daw::expressionFromKey("wobble", read) && daw::expressionName(kTuning) == "Note tuning",
                "Expression: saved by its word, named in the menu");
    }

    // The note tools carry it
    {
        std::vector<Note> notes{tuned(0, 2, 60, {{0, 0.0f}, {2, 2.0f}})};
        daw::slice(notes, 0, 1.0, 1.0 / 64.0);
        c.check(notes.size() == 2 && nearF(notes[0].expressionAt(kTuning, 0.99), 0.99f) &&
                    nearF(notes[1].expressionAt(kTuning, 0.0), 1.0f) && nearF(notes[1].expressionAt(kTuning, 1.0), 2.0f),
                "Expression: slicing a note splits its curve, with no jump at the cut",
                pointsText(notes[0]) + " | " + pointsText(notes[1]));

        daw::join(notes);
        c.check(notes.size() == 1 && nearF(notes[0].expressionAt(kTuning, 0.5), 0.5f) &&
                    nearF(notes[0].expressionAt(kTuning, 1.5), 1.5f),
                "Expression: joining the halves gives the curve back", pointsText(notes[0]));

        std::vector<Note> flat{tuned(0, 2, 60, {{0, 0.0f}}), tuned(2, 2, 60, {{0, 0.0f}})};
        flat[0].setCurve(kTuning, {});
        flat[1].setCurve(kTuning, {});
        daw::join(flat);
        c.check(flat.size() == 1 && flat[0].expression.empty(), "Expression: joining notes without curves adds none");

        std::vector<Note> chopped{tuned(0, 2, 60, {{0, 0.0f}, {2, 4.0f}})};
        daw::chop(chopped, 1.0);
        c.check(chopped.size() == 2 && nearF(chopped[1].expressionAt(kTuning, 0.5), 3.0f),
                "Expression: chopping cuts the curve into each piece", pointsText(chopped[1]));

        std::vector<Note> stretched{tuned(0, 1, 60, {{0, 0.0f}, {1, 1.0f}})};
        daw::stretch(stretched, 2.0);
        c.check(nearF(stretched[0].expressionAt(kTuning, 1.0), 0.5f), "Expression: stretching stretches the curve");

        std::vector<Note> reversed{tuned(0, 1, 60, {{0, 0.0f}, {1, 1.0f}})};
        daw::reverse(reversed);
        c.check(nearF(reversed[0].expressionAt(kTuning, 0.0), 1.0f) && nearF(reversed[0].expressionAt(kTuning, 1.0), 0.0f),
                "Expression: reversing plays the curve backwards", pointsText(reversed[0]));

        std::vector<Note> moved{tuned(1, 1, 60, {{0.5, 3.0f}})};
        daw::transpose(moved, 5);
        daw::nudge(moved, 1.0);
        c.check(pointsText(moved[0]) == pointsText(tuned(0, 1, 60, {{0.5, 3.0f}})),
                "Expression: moving and transposing a note leave its curve as it is");
    }

    // The piano roll's editing
    {
        Note n;
        n.length = 2.0;
        daw::setCurvePoint(n, kTuning, 1.0, 3.0f, 0.1);
        c.check(nearF(n.expressionAt(kTuning, 0.0), 0.0f) && nearF(n.expressionAt(kTuning, 1.0), 3.0f),
                "Expression: a first point inside a note starts the curve at no bend", pointsText(n));
        daw::setCurvePoint(n, kTuning, 1.05, 99.0f, 0.1);
        c.check(n.curve(kTuning)->points.size() == 2 && nearF(n.expressionAt(kTuning, 1.05), 48.0f),
                "Expression: a point replaces the ones near it, clamped to the range", pointsText(n));
        daw::curveLine(n, kTuning, 2.0, -2.0f, 0.0, 0.0f);
        c.check(n.curve(kTuning)->points.size() == 2 && nearF(n.expressionAt(kTuning, 1.0), -1.0f),
                "Expression: a line replaces what was between its ends", pointsText(n));
        daw::shiftCurve(n, kTuning, 2.0f);
        c.check(nearF(n.expressionAt(kTuning, 0.0), 2.0f) && nearF(n.expressionAt(kTuning, 2.0), 0.0f),
                "Expression: shifting moves the whole curve");
        n.setCurve(kTuning, {{0, 1.0f}, {0.5, 1.0f}, {1, 1.0f}});
        daw::tidyCurve(n, kTuning, 0.01f);
        c.check(n.curve(kTuning)->points.size() == 2, "Expression: tidying thins a straight run", pointsText(n));
        daw::shiftCurve(n, kTuning, -1.0f);
        daw::tidyCurve(n, kTuning, 0.01f);
        c.check(n.expression.empty(), "Expression: a curve left at no bend goes");
    }

    // The history sees it and names it
    {
        const daw::Project before = daw::demoProject();
        daw::Project after = before;
        Note *n = firstNote(after);
        c.check(n != nullptr, "Expression: the demo has an instrument note");
        if (n) {
            n->setCurve(kTuning, {{0, 0.0f}, {0.5, 2.0f}});
            c.check(!daw::sameContent(before, after) && daw::describeChange(before, after) == "Note tuning",
                    "Expression: an edit of a curve is a change named after it", daw::describeChange(before, after));
        }
    }

    // Through the file
    {
        const daw::Project demo = daw::demoProject();
        c.check(daw::writeProject(demo).find("expression") == std::string::npos,
                "Expression: notes without curves write no key, as before");

        daw::Project p = demo;
        Note *n = firstNote(p);
        if (n)
            n->setCurve(kTuning, {{0, 0.0f}, {0.25, -1.5f}, {1, 12.0f}});
        const std::string text = daw::writeProject(p);
        daw::Project loaded;
        std::string error;
        const bool read = daw::readProject(text, loaded, error);
        Note *back = read ? firstNote(loaded) : nullptr;
        c.check(read && n && back && back->expression == n->expression && daw::writeProject(loaded) == text,
                "Expression: a curve goes through the file and back", error);

        // By hand: an unknown type dropped, values clamped, points sorted, a second curve of a type dropped
        p = demo;
        n = firstNote(p);
        if (n)
            n->setCurve(kTuning, {{0, 7.0f}});
        std::string hand = daw::writeProject(p);
        const std::string curve = "{ type = \"tuning\", points = [ { beat = 0.0, value = 7.0 } ] }";
        const size_t at = hand.find(curve);
        c.check(at != std::string::npos, "Expression: the curve is written as expected");
        if (at != std::string::npos) {
            hand.replace(at, curve.size(),
                         "{ type = \"wobble\", points = [ { beat = 0, value = 1 } ] }, "
                         "{ type = \"tuning\", points = [ { beat = 1, value = 99 }, { beat = 0, value = -2 } ] }, "
                         "{ type = \"tuning\", points = [ { beat = 0, value = 5 } ] }");
            error.clear();
            daw::Project odd;
            const bool ok = daw::readProject(hand, odd, error);
            Note *o = ok ? firstNote(odd) : nullptr;
            c.check(ok && o && o->expression.size() == 1 && pointsText(*o) == pointsText(tuned(0, 1, 60, {{0, -2.0f}, {1, 48.0f}})),
                    "Expression: unknown types dropped, values clamped, points sorted, one curve a type",
                    o ? pointsText(*o) : error);
        }
    }

    return c.finish("expression");
}

}
