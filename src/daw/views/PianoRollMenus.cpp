#include "daw/views/PianoRollMenus.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <string>
#include <vector>

#include "daw/widgets/Format.h"
#include "daw/views/PianoRoll.h"
#include "daw/model/Project.h"
#include "env/Environment.h"

namespace daw {

using namespace ui;

PianoRollMenus::PianoRollMenus(env::Environment &env, Context &gui, PianoRollView &roll) : env_(env), gui_(gui), roll_(roll) {}

std::vector<MenuItem> PianoRollMenus::noteMenu()
{
    PianoRollView *roll = &roll_;
    auto tool = [roll](const char *label, std::function<int(std::vector<Note> &)> run, const char *action = "") {
        return MenuItem::item(label, [roll, run, label] { roll->edit(run, label); }, action);
    };
    auto hasClip = [roll] { return roll->clip() != nullptr; };
    auto hasSelection = [roll] { return roll->hasSelectedNote(); };
    const double grid = roll->snap();
    const Scale *scale = roll->scale();
    const int root = roll->root();
    const uint32_t seed = static_cast<uint32_t>(env_.frame()) * 2654435761u + 1u;
    return {
        MenuItem::item("Select all", [roll] { roll->selectAll(); }, "edit.selectAll").when(hasClip),
        MenuItem::item("Deselect", [roll] { roll->edit([](std::vector<Note> &n) { selectAll(n, false); return 0; }, "Deselected"); },
                       "notes.deselect").when(hasSelection),
        MenuItem::divider(),
        MenuItem::item("Cut", [roll] { roll->cut(); }, "edit.cut").when(hasSelection),
        MenuItem::item("Copy", [roll] { roll->copy(); }, "edit.copy").when(hasSelection),
        MenuItem::item("Paste", [roll] { roll->paste(); }, "edit.paste").when(hasClip),
        MenuItem::item("Duplicate", [roll] { roll->duplicate(); }, "clip.duplicate").when(hasSelection),
        MenuItem::item("Delete", [roll] { roll->deleteSelectedNote(); }, "edit.delete").when(hasSelection),
        MenuItem::item("Mute or unmute", [roll] { roll->toggleMute(); }, "notes.mute").when(hasClip),
        MenuItem::item("Select the same keys", [roll] {
            roll->edit([](std::vector<Note> &n) { return selectSameKeys(n); }, "Selected the same keys");
        }).when(hasSelection),
        MenuItem::item("Loop these notes", [roll] { roll->loopSelection(); }, "notes.loop").when(hasClip),
        MenuItem::divider(),
        tool("Quantize", [grid](std::vector<Note> &n) { return quantize(n, grid); }, "notes.quantize").when(hasClip),
        tool("Quantize halfway", [grid](std::vector<Note> &n) { return quantize(n, grid, 0.5); }).when(hasClip),
        tool("Quantize starts and ends", [grid](std::vector<Note> &n) { return quantize(n, grid, 1.0, true); }).when(hasClip),
        tool("Legato", [](std::vector<Note> &n) { return legato(n); }).when(hasClip),
        tool("Fix overlaps", [](std::vector<Note> &n) { return fixOverlaps(n); }).when(hasClip),
        tool("Humanize", [grid, seed](std::vector<Note> &n) { return humanize(n, grid * 0.08, 0.08f, seed); }).when(hasClip),
        MenuItem::sub("Velocity", {
            tool("Louder", [](std::vector<Note> &n) { return shiftVelocity(n, 8.0f / 127.0f); }, "notes.louder"),
            tool("Softer", [](std::vector<Note> &n) { return shiftVelocity(n, -8.0f / 127.0f); }, "notes.softer"),
            MenuItem::divider(),
            tool("Set to 64", [](std::vector<Note> &n) { return setVelocity(n, 64.0f / 127.0f); }),
            tool("Set to 100", [](std::vector<Note> &n) { return setVelocity(n, 100.0f / 127.0f); }),
            tool("Set to 127", [](std::vector<Note> &n) { return setVelocity(n, 1.0f); }),
            MenuItem::divider(),
            tool("Compress", [](std::vector<Note> &n) { return scaleVelocity(n, 0.6f); }),
            tool("Expand", [](std::vector<Note> &n) { return scaleVelocity(n, 1.5f); }),
            tool("Crescendo", [](std::vector<Note> &n) { return rampVelocity(n, 40.0f / 127.0f, 1.0f); }),
            tool("Diminuendo", [](std::vector<Note> &n) { return rampVelocity(n, 1.0f, 40.0f / 127.0f); }),
            tool("Randomize", [seed](std::vector<Note> &n) { return humanize(n, 0.0, 0.15f, seed); }),
        }).when(hasClip),
        MenuItem::sub("Transform", {
            tool("Up an octave", [](std::vector<Note> &n) { return transpose(n, 12); }, "notes.octaveUp"),
            tool("Down an octave", [](std::vector<Note> &n) { return transpose(n, -12); }, "notes.octaveDown"),
            tool("Fit to scale", [scale, root](std::vector<Note> &n) { return scale ? fitToScale(n, root, *scale) : 0; })
                .when([roll] { return roll->scale() != nullptr; }),
            MenuItem::divider(),
            tool("Reverse", [](std::vector<Note> &n) { return reverse(n); }),
            tool("Invert", [](std::vector<Note> &n) { return invert(n); }),
            tool("Double length", [](std::vector<Note> &n) { return stretch(n, 2.0); }),
            tool("Half length", [](std::vector<Note> &n) { return stretch(n, 0.5); }),
            MenuItem::divider(),
            tool("Chop to the grid", [grid](std::vector<Note> &n) { return chop(n, grid); }),
            tool("Join", [](std::vector<Note> &n) { return join(n); }),
        }).when(hasClip),
        MenuItem::divider(),
        MenuItem::item("Arpeggiate...", [this] { openArpeggiator(); }).when(hasClip),
    };
}

std::vector<MenuItem> PianoRollMenus::gridMenu()
{
    PianoRollView *roll = &roll_;
    std::vector<MenuItem> items;
    const std::vector<std::string> &choices = PianoRollView::gridChoices();
    for (size_t i = 0; i < choices.size(); ++i) {
        const int choice = static_cast<int>(i);
        items.push_back(MenuItem::toggle(i == 0 ? "Auto (follows the zoom)" : choices[i], [roll, choice] { return roll->grid() == choice; },
                                         [roll, choice] { roll->setGrid(choice); }));
    }
    items.push_back(MenuItem::divider());
    items.push_back(MenuItem::toggle("Triplets", [roll] { return roll->triplet(); }, [roll] { roll->setTriplet(!roll->triplet()); }));
    items.push_back(MenuItem::toggle("Snap to the grid", [roll] { return roll->snapping(); },
                                     [roll] { roll->setSnapping(!roll->snapping()); }));
    return items;
}

std::vector<MenuItem> PianoRollMenus::scaleMenu()
{
    PianoRollView *roll = &roll_;
    std::vector<MenuItem> items{
        MenuItem::toggle("No scale", [roll] { return roll->scaleIndex() < 0; }, [roll] { roll->setScale(-1); })};
    for (size_t i = 0; i < scales().size(); ++i) {
        const int index = static_cast<int>(i);
        items.push_back(MenuItem::toggle(scales()[i].name, [roll, index] { return roll->scaleIndex() == index; },
                                         [roll, index] { roll->setScale(index); }));
    }
    std::vector<MenuItem> roots;
    for (int r = 0; r < 12; ++r)
        roots.push_back(MenuItem::toggle(rootName(r), [roll, r] { return roll->root() == r; }, [roll, r] { roll->setRoot(r); }));
    items.push_back(MenuItem::divider());
    items.push_back(MenuItem::sub("Root", roots));
    items.push_back(MenuItem::toggle("Keep notes in key", [roll] { return roll->inKey(); }, [roll] { roll->setInKey(!roll->inKey()); })
                        .when([roll] { return roll->scale() != nullptr; }));
    items.push_back(MenuItem::toggle("Fold to the scale", [roll] { return roll->fold(); }, [roll] { roll->setFold(!roll->fold()); }));
    return items;
}

std::vector<MenuItem> PianoRollMenus::rollViewMenu()
{
    PianoRollView *roll = &roll_;
    using Ghosts = PianoRollView::Ghosts;
    auto ghost = [roll](const char *label, Ghosts g) {
        return MenuItem::toggle(label, [roll, g] { return roll->ghosts() == g; }, [roll, g] { roll->setGhosts(g); });
    };
    return {
        MenuItem::sub("Ghost notes", {ghost("None", Ghosts::Off), ghost("This track's other clips", Ghosts::Track),
                                      ghost("Every instrument track", Ghosts::All)}),
        MenuItem::toggle("Lane under the notes", [roll] { return roll->velocityLane(); },
                         [roll] { roll->setVelocityLane(!roll->velocityLane()); }),
        MenuItem::sub("Lane shows", laneMenu()),
        MenuItem::toggle("Fold rows (the scale, or the keys in use)", [roll] { return roll->fold(); }, [roll] { roll->setFold(!roll->fold()); }),
        MenuItem::divider(),
        MenuItem::toggle("Hear notes as they're edited", [roll] { return roll->audition(); }, [roll] { roll->setAudition(!roll->audition()); }),
        MenuItem::toggle("Follow the playhead", [roll] { return roll->follow(); }, [roll] { roll->setFollow(!roll->follow()); }),
        MenuItem::divider(),
        MenuItem::item("Zoom to fit", [roll] { roll->fit(); }, "notes.fit"),
        MenuItem::item("Loop these notes", [roll] { roll->loopSelection(); }, "notes.loop").when([roll] { return roll->clip() != nullptr; }),
    };
}

std::vector<MenuItem> PianoRollMenus::laneMenu()
{
    PianoRollView *roll = &roll_;
    // A lane's name, marked when the clip has a curve for it
    auto used = [roll](int controller) {
        const Clip *c = roll->clip();
        if (!c)
            return false;
        if (controller == PianoRollView::kTuningLane)
            return std::any_of(c->notes.begin(), c->notes.end(), [](const Note &n) { return n.curve(Expression::Tuning) != nullptr; });
        for (const ControlLane &lane : c->controls) {
            if (lane.controller == controller && !lane.points.empty())
                return true;
        }
        return false;
    };
    auto lane = [roll, used](int controller) {
        const std::string name = controller == PianoRollView::kVelocityLane ? "Velocity"
                                 : controller == PianoRollView::kTuningLane ? expressionName(Expression::Tuning)
                                                                            : controllerName(controller);
        return MenuItem::toggle(name, [roll, controller] { return roll->lane() == controller; }, [roll, controller] {
            roll->setLane(controller);
            roll->setVelocityLane(true);
        }).labelled([name, used, controller] { return used(controller) ? name + "  *" : name; });
    };
    std::vector<MenuItem> items{lane(PianoRollView::kVelocityLane), lane(PianoRollView::kTuningLane), MenuItem::divider()};
    for (int controller : {1, kPitchBend, 11, 7, 64, 10, 2, 74, 71})
        items.push_back(lane(controller));
    // Every other controller, in groups
    std::vector<MenuItem> others;
    for (int group = 0; group < 128; group += 32) {
        std::vector<MenuItem> part;
        for (int controller = group; controller < group + 32; ++controller)
            part.push_back(lane(controller));
        others.push_back(MenuItem::sub(format("CC %.0f", group) + format(" to %.0f", group + 31), part));
    }
    items.push_back(MenuItem::sub("Other controllers", others));
    items.push_back(MenuItem::divider());
    items.push_back(MenuItem::item("Clear this lane", [roll] { roll->clearLane(); })
                        .labelled([roll] { return "Clear " + roll->laneName(); })
                        .when([roll, used] { return roll->lane() != PianoRollView::kVelocityLane && used(roll->lane()); }));
    return items;
}

std::vector<MenuItem> PianoRollMenus::insertMenu()
{
    PianoRollView *roll = &roll_;
    auto hasClip = [roll] { return roll->clip() != nullptr; };
    std::vector<MenuItem> chords;
    for (const ChordShape &shape : chordShapes()) {
        const ChordShape copy = shape;
        chords.push_back(MenuItem::item(shape.name, [roll, copy] { roll->insertChord(copy); }));
    }
    return {
        MenuItem::sub("Scale run", {
            MenuItem::item("Up an octave", [roll] { roll->insertScaleRun(1, 1); }),
            MenuItem::item("Up two octaves", [roll] { roll->insertScaleRun(1, 2); }),
            MenuItem::item("Down an octave", [roll] { roll->insertScaleRun(-1, 1); }),
            MenuItem::item("Up and back down", [roll] { roll->insertScaleRun(0, 1); }),
        }).when(hasClip),
        MenuItem::sub("Chord", chords).when(hasClip),
    };
}

void PianoRollMenus::openArpeggiator()
{
    if (!roll_.clip())
        return;
    // The window the click came from (the piano roll may be floating in its own window)
    daw::openArpeggiator(ui::Context::activeOr(gui_), arp_, [this] {
        const Clip *c = roll_.clip();
        return c ? c->notes : std::vector<Note>{};
    }, [this] { roll_.arpeggiate(arp_); });
}

}

