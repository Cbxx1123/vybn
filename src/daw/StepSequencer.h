#pragma once

#include <functional>
#include <string>
#include <vector>

#include "Project.h"
#include "ProjectStore.h"
#include "StepSequence.h"
#include "ui/UI.h"

namespace daw {

// What the step sequencer reaches outside the project. Positions are in beats on the timeline.
struct StepSequencerHooks {
    std::function<void(int key, float velocity)> auditionOn;
    std::function<void(int key)> auditionOff;
    std::function<double()> position;
    std::function<bool()> playing;
    // The menu of a row's tools (built by the app: StepMenus), from a right-click on its name or a cell
    std::function<void(int key, ui::Vec2 at)> rowMenu;
    // The menu of rows to add (StepMenus), from the "Add row" line under the rows
    std::function<void(ui::Vec2 at)> addRowMenu;
};

// A drum machine's view of the selected clip of an instrument track (the editor's other page beside
// the piano roll): a row per key (the GM kit, then any other key the clip uses, and rows added),
// the row's name down the left, its steps across, and a velocity lane under the grid for the
// selected row. A step is on while a note of its key starts in it (StepSequence.h), so every edit
// is an ordinary note edit through ProjectStore::editClip, undoable like the piano roll's.
// - Press a step to turn it on (a note gate steps long at the view's velocity) or off; drag along
//   or across rows to paint the same state. Turning one on plays its key when hearing is on.
// - Press a row's name to select it (the velocity lane shows it) and hear its key; right-click a
//   name or a step for the row's menu (hooks.rowMenu). "Add row" under the rows opens
//   hooks.addRowMenu.
// - In the velocity lane drag a step's bar to set its velocity, or across to paint.
// - The wheel scrolls the rows, Shift-wheel the steps (when they don't all fit; a step is at
//   least kMinStepWidth wide). Beats are shaded in alternate groups; the step under the playhead
//   is lit while playing.
// Drawing follows the Theme only (inset wells for the grid, raised accent-filled pills for steps
// that are on, brighter by velocity, hollow for muted rows).
class StepSequencerView : public ui::Component {
public:
    static constexpr float kNameWidth = 112.0f;
    static constexpr float kHeaderHeight = 22.0f;
    static constexpr float kRowHeight = 26.0f;
    static constexpr float kLaneHeight = 64.0f;
    static constexpr float kMinStepWidth = 18.0f;

    StepSequencerView(ProjectStore &store, StepSequencerHooks hooks);

    // The clip being edited (the selection, on an instrument track), or null
    const Clip *clip() const;
    // The grid over the clip now (stepGrid of its length, the step size and the swing); a 4-beat
    // grid when there's no clip
    StepGrid grid() const;

    // How it is set, kept between runs (Settings' `stepSequencer` line)
    struct ViewSettings {
        int step = kDefaultStepChoice; // index into stepChoices()
        double swing = 0.0;            // 0-1
        double gate = 0.5;             // of a step, 0.05-1, for new steps
        float velocity = 0.8f;         // for new steps, 0.01-1
        bool audition = true;          // hear steps as they are turned on, and rows' names
        bool operator==(const ViewSettings &o) const;
        bool operator!=(const ViewSettings &o) const { return !(*this == o); }
    };
    ViewSettings view() const { return view_; }
    void setView(const ViewSettings &view); // clamps every field
    // One line of `key=value` pairs separated by spaces ("step=1 swing=0.25 gate=0.5 velocity=0.8
    // audition=1"); read back with unknown keys ignored and missing or unreadable ones at defaults
    static std::string writeView(const ViewSettings &view);
    static ViewSettings readView(const std::string &text);

    int stepChoice() const { return view_.step; }
    void setStepChoice(int index);
    double swing() const { return view_.swing; }
    // Sets the swing and re-places the clip's notes for it (applySwing), as one clip edit
    void setSwing(double swing);
    double gate() const { return view_.gate; }
    void setGate(double gate);
    float velocity() const { return view_.velocity; }
    void setVelocity(float velocity);
    bool audition() const { return view_.audition; }
    void setAudition(bool on);

    // The rows shown, top to bottom: stepRows(the clip's notes, the kit and the rows added)
    std::vector<int> rows() const;
    // Shows a row for `key` (1-127) even while it is empty (kept for the session, for every clip)
    void addRow(int key);
    // Clears the row's notes in this clip and stops showing it (a kit row too) unless a note still
    // uses it
    void removeRow(int key);
    int selectedRow() const { return selectedRow_; } // a key, or -1
    void selectRow(int key);

    // Row tools, each one edit of the clip (no clip: nothing), for the menus
    void clearRow(int key);
    void shiftRow(int key, int steps);
    void fillRow(int key, int every, int offset);           // everyPattern at the view's velocity
    void euclidRow(int key, int pulses, int rotation);      // euclidean over the grid's count
    void randomRow(int key, double density, uint32_t seed); // randomPattern at the view's velocity
    bool rowMuted(int key) const;
    void toggleRowMute(int key);
    void copyRow(int key);  // the row's pattern into a clipboard shared by every row
    void pasteRow(int key); // setRow with the copied pattern (cut or padded to the grid)
    bool canPasteRow() const { return !clipboard_.empty(); }

    // What the mouse can do where it is ("Click to turn a step on or off, drag to paint"), for the
    // editor's header; and the last thing done ("Filled Kick every 4 steps"), cleared after 3 s
    std::string hint() const { return hint_; }
    std::string message() const;

    ui::Vec2 measure(const ui::Theme &theme) const override;
    void arrange(const ui::Rect &rect, const ui::Theme &theme) override;
    void handleInput(const ui::Input &input) override;
    void draw(ui::DrawList &list, const ui::Theme &theme) const override;

private:
    const Project &project() const { return store_.project(); }
    // The selected clip's notes to change, or null (no clip); one call per edit
    std::vector<Note> *editNotes();
    void say(std::string text);

    ProjectStore &store_;
    StepSequencerHooks hooks_;
    ViewSettings view_;
    std::vector<int> added_;        // rows added by hand, shown while empty
    std::vector<int> hidden_;       // kit rows removed by hand, hidden while empty
    std::vector<float> clipboard_;  // copyRow's pattern
    int selectedRow_ = -1;
    bool addHot_ = false;           // the mouse is over the "Add row" line

    // Layout, set in arrange
    ui::Rect names_, header_, grid_, lane_, addRow_;
    float stepWidth_ = kMinStepWidth;
    float scrollX_ = 0.0f, scrollY_ = 0.0f;

    // The press under way
    enum class Drag { None, Paint, Lane, Name };
    Drag drag_ = Drag::None;
    bool paintOn_ = false;
    int auditioning_ = -1;   // the key held for audition, or -1
    double auditionUntil_ = 0.0;
    std::string hint_;
    std::string message_;
    double messageTime_ = 0.0;
    double now_ = 0.0;
};

}
