#pragma once

#include <functional>
#include <vector>

#include "daw/model/NoteTools.h"
#include "ui/UI.h"

namespace daw {

// The arpeggiator's dialog: a preset, the pattern, its rate, octaves, gate, swing, ratchets and
// transposing, velocity and accents, humanizing, the rhythm's steps (click a step: play, accent,
// tie, rest), and a preview of what it will make from the clip's notes, redrawn as anything
// changes. `settings` is edited in place (the app keeps it between uses); `notes` gives the
// clip's notes as they are (the preview works on a copy); `apply` runs on Arpeggiate.
void openArpeggiator(ui::Context &context, ArpSettings &settings, std::function<std::vector<Note>()> notes,
                     std::function<void()> apply);

// The rhythm's steps, one cell each: raised to play, filled with the accent when accented,
// joined to the one before when tied, pressed in for a rest. A click moves a step on through
// those four.
class ArpSteps : public ui::Component {
public:
    explicit ArpSteps(std::string &rhythm) : rhythm_(rhythm) {}

    ui::Vec2 measure(const ui::Theme &theme) const override;
    void handleInput(const ui::Input &input) override;
    void draw(ui::DrawList &list, const ui::Theme &theme) const override;

private:
    ui::Rect cell(size_t index) const;

    std::string &rhythm_;
    int hovered_ = -1;
};

// What the arpeggiator will make: its notes bright over the notes it starts from, faint, in an
// inset well, the first two bars of it at most, with a line on each step
class ArpPreview : public ui::Component {
public:
    ArpPreview(const ArpSettings &settings, std::function<std::vector<Note>()> notes)
        : settings_(settings), notes_(std::move(notes))
    {
    }

    ui::Vec2 measure(const ui::Theme &) const override { return {520.0f, 150.0f}; }
    void handleInput(const ui::Input &input) override;
    void draw(ui::DrawList &list, const ui::Theme &theme) const override;

private:
    const ArpSettings &settings_;
    std::function<std::vector<Note>()> notes_;
    std::vector<Note> before_, after_;
    double from_ = 0.0, to_ = 4.0;
    int low_ = 60, high_ = 72;
};

}
