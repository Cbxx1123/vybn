#pragma once

#include <functional>
#include <string>
#include <vector>

#include "NoteTools.h"
#include "Project.h"
#include "ProjectStore.h"
#include "ui/DrawCache.h"
#include "ui/UI.h"

namespace daw {

// What the piano roll reaches outside the project: previewing keys, the transport, and the menu
// of note tools (built by the app, opened on a right-click). Positions are in beats on the
// timeline.
struct PianoRollHooks {
    std::function<void(int key, float velocity)> auditionOn;
    std::function<void(int key)> auditionOff;
    std::function<double()> position;
    std::function<void(double)> seek;
    std::function<bool()> playing;
    std::function<void(ui::Vec2 at)> contextMenu;
    // The menu of lanes (velocity, the controllers), from the lane's name
    std::function<void(ui::Vec2 at)> laneMenu;
};

// The note editor for the selected clip of an instrument track: a keyboard down the left, a ruler
// along the top (a loop strip over the bars, with the clip's start and end as handles), the clip's
// notes on a grid of keys and beats, a velocity lane, and an overview of the whole clip along the
// bottom.
// Tools (the toolbar, or B, V, C, E):
// - Draw: press an empty cell to add a note (snapped, with the length and velocity of the last
//   one edited) and drag right to set its length, up or down for its key. Ctrl (or Shift) drags a
//   box instead.
// - Select: drag a box (Shift adds; Alt draws a lasso).
// - Slice: press a note to cut it at the grid under the mouse (the selection, if it is selected).
// - Erase: press or drag over notes to delete them.
// With Draw or Select: press a note to select it (Shift toggles it); drag to move the selection
// (Alt drags copies; Ctrl ignores the grid); drag either end to change the selection's lengths or
// starts; double-click a note to delete it; right-click for the note tools. With two or more
// notes selected, their bounds show handles at each side that stretch their timing.
// Lanes: the lane shows the notes' velocity or a controller's curve (its name opens the menu of
// lanes). In the velocity lane drag a note's handle (the selection follows by as much), drag
// across to paint, or Alt-drag a straight ramp. In a controller's lane drag across to draw its
// curve (on the grid; thinned when let go), drag a point to move it, double-click one to delete
// it, Alt-drag a straight line, or erase with the Erase tool. The bar above the lane resizes it.
// Ghost notes: the track's other clips (or every instrument track's) drawn faintly where they
// sound; double-click one to edit its clip.
// The view: the wheel scrolls the keys, Shift-wheel time, Ctrl-wheel zooms time around the mouse,
// Alt-wheel the row height; zooming and scrolling glide. Middle-drag pans. Dragging past an edge
// scrolls. With Follow it pages to keep the playhead in view while playing. Fold shows only the
// scale's keys (or, with no scale, the keys the clip uses).
// The loop (the project's): drag along the loop strip to set a range (its ends drag; a click in
// it turns looping on or off); L sets it to the selection. The app makes the transport loop.
// While it is showing it hooks edit.delete, edit.selectAll, edit.copy/cut/paste, clip.duplicate
// (with notes selected) and the notes.* actions: up/down, octaveUp/octaveDown, earlier/later,
// nudgeEarlier/nudgeLater, shorter/longer, previous/next, louder/softer, quantize, deselect,
// mute, loop, fit, and the tools. With no clip (or for the selection ones, no selection) they go
// on.
class PianoRollView : public ui::Component {
public:
    enum class Tool { Draw, Select, Slice, Erase };
    enum class Ghosts { Off, Track, All };
    static constexpr int kVelocityLane = -1;

    // How the editor looks, kept between runs (Settings)
    struct ViewSettings {
        int grid = 0;
        bool triplet = false;
        bool snapping = true;
        Ghosts ghosts = Ghosts::Track;
        bool fold = false;
        bool laneShown = true;
        int lane = kVelocityLane;
        float laneHeight = 76.0f;
        bool audition = true;
        bool follow = true;
        bool inKey = false;
        int scale = -1;
        int root = 0;
        float zoom = 48.0f;      // pixels a beat
        float rowHeight = 14.0f;
        bool operator==(const ViewSettings &o) const;
        bool operator!=(const ViewSettings &o) const { return !(*this == o); }
    };
    ViewSettings view() const;
    void setView(const ViewSettings &view);
    // As one line of `key=value` pairs separated by spaces ("grid=5 triplet=0 lane=1 ..."); read
    // back with unknown keys ignored and missing ones at their defaults
    static std::string writeView(const ViewSettings &view);
    static ViewSettings readView(const std::string &text);

    PianoRollView(ProjectStore &store, PianoRollHooks hooks);

    // The grid: an index into gridChoices() (0 is Auto: it follows the zoom), triplets, and
    // whether edits snap to it (Ctrl does the opposite while held)
    static const std::vector<std::string> &gridChoices();
    void setGrid(int choice) { gridChoice_ = choice; }
    int grid() const { return gridChoice_; }
    void setTriplet(bool on) { triplet_ = on; }
    bool triplet() const { return triplet_; }
    void setSnapping(bool on) { snapping_ = on; }
    bool snapping() const { return snapping_; }
    // The grid's length in beats now (what quantize and the arrows use)
    double snap() const;
    std::string gridText() const;

    void setTool(Tool tool) { tool_ = tool; }
    Tool tool() const { return tool_; }
    void zoom(float factor);
    // Zooms to show the selection (or the whole clip when nothing is selected)
    void fit();
    // Scrolls to show the selected clip's notes from its start, at the next layout
    void reveal() { revealPending_ = true; }
    // The clip being edited, or nullptr when the selection isn't an instrument clip
    const Clip *clip() const;
    bool hasSelectedNote() const;
    int selectedCount() const;
    void deleteSelectedNote();

    // The scale shown: an index into scales() (-1 for none) and its root (0 C ... 11 B); with
    // inKey, new and dragged notes land on the scale
    void setScale(int index) { scale_ = index; }
    int scaleIndex() const { return scale_; }
    void setRoot(int root) { root_ = ((root % 12) + 12) % 12; }
    int root() const { return root_; }
    void setInKey(bool on) { inKey_ = on; }
    bool inKey() const { return inKey_; }
    const Scale *scale() const;
    void setVelocityLane(bool shown) { velocityLane_ = shown; }
    bool velocityLane() const { return velocityLane_; }
    // What the lane shows: kVelocityLane, or a controller (0-127, kPitchBend)
    void setLane(int controller) { laneController_ = controller; }
    int lane() const { return laneController_; }
    std::string laneName() const;
    // Removes the clip's curve for the lane shown
    void clearLane();
    void setGhosts(Ghosts ghosts) { ghosts_ = ghosts; }
    Ghosts ghosts() const { return ghosts_; }
    void setFold(bool on) { fold_ = on; }
    bool fold() const { return fold_; }
    void setFollow(bool on) { follow_ = on; }
    bool follow() const { return follow_; }
    // Whether pressing and moving notes plays them
    void setAudition(bool on) { auditionOn_ = on; }
    bool audition() const { return auditionOn_; }

    // The project's loop, in beats on the timeline, and whether the transport loops in it
    double loopStart() const { return project().loopStart; }
    double loopEnd() const { return project().loopEnd; }
    bool hasLoop() const { return project().hasLoop(); }
    void setLoop(double start, double end);
    void setLooping(bool on) { store_.setLoop(project().loopStart, project().loopEnd, on && hasLoop()); }
    bool looping() const { return project().looping; }
    // The loop set to the selected notes (or the clip), rounded out to the grid, and turned on
    void loopSelection();

    // Runs a note tool on the clip's notes (it acts on the selection, or every note when none is
    // selected; see NoteTools), keeps them inside the clip, and says what happened under the
    // toolbar. Returns false when there is no clip.
    bool edit(const std::function<int(std::vector<Note> &)> &tool, const std::string &done);
    // New notes at the insertion point (the playhead when it is in the clip, else after the
    // selection, else the clip's start), from the lowest selected key (else the scale's root
    // around C4): a run of the scale, or a chord. They become the selection.
    void insertScaleRun(int direction, int octaves);
    void insertChord(const ChordShape &shape);
    void arpeggiate(const ArpSettings &settings);
    void copy();
    void cut();
    void paste();
    void selectAll();
    void duplicate();
    void toggleMute();

    // The selection, for the inspector: the first selected note in time (null when none), and
    // changes to every selected note by steps (Shift-sized steps are the caller's)
    const Note *firstSelected() const;
    void transposeSelection(int semitones);
    void nudgeSelection(double beats);
    void resizeSelection(double beats);
    void velocitySelection(float by);

    // The last thing done, for a status line
    const std::string &message() const { return message_; }
    // What the mouse can do now, for a hint line
    std::string hint() const;
    // The velocity under the mouse or being dragged (0-1), or -1
    float shownVelocity() const { return laneController_ == kVelocityLane ? shownVelocity_ : -1.0f; }

    ui::Vec2 measure(const ui::Theme &theme) const override;
    void arrange(const ui::Rect &rect, const ui::Theme &theme) override;
    void handleInput(const ui::Input &input) override;
    void draw(ui::DrawList &list, const ui::Theme &theme) const override;

private:
    // The project, for reading
    const Project &project() const { return store_.project(); }
    // The clip being edited, counted as changed now (see ProjectStore), or nullptr without one
    Clip *editClip();

    enum class Drag {
        None, Draw, Move, Resize, ResizeStart, Stretch, Keys, Ruler, Loop, LoopStart, LoopEnd, ClipStart, ClipEnd,
        Mini, Box, Lasso, Pan, Velocity, Paint, Ramp, Erase, Divider, Point, CcPaint, CcLine, CcErase
    };
    struct Ghost {
        double start, length; // clip-relative beats
        int key;
        int track, clip;
        bool sameTrack;
    };

    float beatX(double beat) const; // clip-relative beats
    double xBeat(float x) const;
    float keyY(int key) const; // top of the key's row
    int yKey(float y) const;
    float rowsHeight() const;
    float velocityY(float velocity) const;
    float yVelocity(float y) const;
    ui::Rect noteRect(const Note &note) const;
    // The note under p, and whether p is on its start or end edge (-1, 1, or 0 for the body)
    int noteAt(ui::Vec2 p, int &edge) const;
    int handleAt(ui::Vec2 p) const; // the velocity handle under p
    int pointAt(ui::Vec2 p) const;  // the controller point under p
    const ControlLane *shownLane() const;
    float yValue(float y) const;    // a controller's value at a height in the lane, 0-1
    std::string valueText(float value) const;
    void pressLane(const ui::Input &input);
    void drawControls(ui::DrawList &list, const ui::Theme &theme, const Clip &c) const;
    int ghostAt(ui::Vec2 p) const;
    // Snapping, unless `free` (Ctrl held, or snapping off)
    double snapDown(double beat, bool free = false) const;
    double snapRound(double beat, bool free = false) const;
    bool freeOf(const ui::Input &input) const;
    float contentWidth() const;
    ui::Rect miniThumb() const;
    ui::Rect boundsRect() const; // the selection's bounds, with its stretch handles beside it
    ui::Rect stretchHandle(int side) const;
    ui::Rect clipHandle(int side) const;
    ui::Rect loopRect() const;
    void clampScroll();
    void setScrollX(float x, bool glide);
    void setScrollY(float y, bool glide);
    void zoomAround(float factor, float x);
    void zoomRows(float factor, float y);
    void glide(const ui::Input &input);
    void audition(int key, float velocity);
    void stopAudition();
    void checkSelection();
    void rebuildRows();
    void rebuildGhosts();
    void scrollToNotes();
    void makeHooks();
    void startDrag(Drag drag, const ui::Input &input, int anchor);
    void updateDrag(const ui::Input &input);
    void finishDrag();
    void pressGrid(const ui::Input &input);
    void hover(const ui::Input &input);
    void eraseAt(ui::Vec2 p);
    // The box being dragged: from where it was pressed (kept on the content as it scrolls) to the mouse
    ui::Rect boxRect() const;
    double insertionPoint() const;
    int insertionKey() const;
    void addNotes(std::vector<Note> made, const std::string &what);
    // What a drag is doing, for the readout beside the mouse
    std::string readout() const;
    // The draw, in three parts. The keys (the corner and the keyboard) are cached on what they
    // read, the playing keys included; the body (the grid, notes, ruler, lane and overview) on
    // everything else it reads; the overlay is drawn every frame on top: the lane's value under
    // the mouse, the overview's and the main playhead, and the readout beside the mouse.
    void playingKeys(const Clip &c, bool (&playing)[128]) const; // the keys under the playhead, while it plays
    uint64_t keysKey(bool hasClip, const bool (&playing)[128]) const;
    uint64_t bodyKey(const Clip *c) const;
    unsigned hotBits(const Clip *c) const; // the mouse over the small controls it lights, when nothing is dragged
    void drawKeys(ui::DrawList &list, const ui::Theme &theme, const Clip *c, const bool (&playing)[128]) const;
    void drawBody(ui::DrawList &list, const ui::Theme &theme, const Clip *c) const;
    void drawOverlay(ui::DrawList &list, const ui::Theme &theme, const Clip *c) const;
    void drawKeyboard(ui::DrawList &list, const ui::Theme &theme, const bool (&playing)[128]) const;
    void drawGrid(ui::DrawList &list, const ui::Theme &theme, const Clip &c) const;
    void drawNotes(ui::DrawList &list, const ui::Theme &theme, const Clip &c) const;
    void drawRuler(ui::DrawList &list, const ui::Theme &theme, const Clip &c) const;
    void drawLane(ui::DrawList &list, const ui::Theme &theme, const Clip &c) const;
    void drawOverview(ui::DrawList &list, const ui::Theme &theme, const Clip &c) const;
    void drawOverviewPlayhead(ui::DrawList &list, const ui::Theme &theme, const Clip &c) const;

    ProjectStore &store_;
    PianoRollHooks hooks_;
    ui::DrawCache keysCache_{"PianoRollKeys"}; // the corner and the keyboard, while what they show stays
    ui::DrawCache cache_{"PianoRollView"};     // the body, while nothing it reads has changed
    Tool tool_ = Tool::Draw;
    int gridChoice_ = 0;
    bool triplet_ = false;
    bool snapping_ = true;
    float pixelsPerBeat_ = 48.0f;
    float rowHeight_ = 14.0f;
    float scrollX_ = 0.0f;
    float scrollY_ = -1.0f; // set on the first layout, to centre on the guitar's range
    // Where gliding scrolls and zooms are heading, and the point a zoom keeps under the mouse
    float targetX_ = 0.0f, targetY_ = 0.0f, targetZoom_ = 48.0f;
    float zoomAtX_ = 0.0f;
    double zoomAtBeat_ = 0.0;
    bool zooming_ = false;
    ui::Rect corner_, keys_, ruler_, loopBand_, grid_, divider_, lane_, laneKeys_, mini_, gridLabel_;
    float laneHeight_ = 76.0f;
    bool revealPending_ = true;
    bool velocityLane_ = true;
    int scale_ = -1;
    int root_ = 0;
    bool inKey_ = false;
    Ghosts ghosts_ = Ghosts::Track;
    bool fold_ = false;
    bool follow_ = true;
    bool auditionOn_ = true;
    int laneController_ = kVelocityLane;
    int hoverPoint_ = -1;
    bool laneMenuPressed_ = false;
    ControlLane beforeLane_;   // the lane as it was, while drawing a line in it
    double paintBeat_ = 0.0;   // where controller painting was last frame
    float paintValue_ = 0.0f;

    // The rows shown, top to bottom (every key, or with fold the scale's or the clip's), and each
    // key's row (a key not shown sits on the row of the next one down)
    std::vector<int> rows_;
    int rowOf_[128] = {};
    std::vector<Ghost> ghostNotes_;

    // The clip the selection was on last frame; editing state resets when it changes
    int clipTrack_ = -1;
    int clipIndex_ = -1;
    int hovered_ = -1;
    int hoverEdge_ = 0;
    int hoverHandle_ = -1;
    int hoverGhost_ = -1;
    int hoverStretch_ = 0; // -1 or 1: a stretch handle under the mouse
    int hoverKey_ = -1;    // the row under the mouse
    double hoverBeat_ = -1.0;
    ui::Vec2 mouse_{-1e9f, -1e9f};
    bool overGrid_ = false;
    bool ctrlHeld_ = false;
    Drag drag_ = Drag::None;
    // While dragging: the note pressed, the mouse then, the notes as they were, the scroll then
    int anchor_ = -1;
    ui::Vec2 pressMouse_;
    ui::Vec2 dragMouse_;
    double pressBeat_ = 0.0;
    int pressKey_ = 0;
    float pressVelocity_ = 0.0f;
    std::vector<Note> before_;
    NoteBounds pressBounds_;
    double pressClipStart_ = 0.0, pressClipLength_ = 0.0;
    double pressLoopStart_ = 0.0, pressLoopEnd_ = 0.0;
    float pressLane_ = 0.0f;
    float panX_ = 0.0f, panY_ = 0.0f;
    bool additive_ = false;      // a Shift box keeps the selection it started with
    bool dragMoved_ = false;     // the mouse has gone a few pixels since the press
    float grabX_ = 0.0f;         // mouse offset into the overview's thumb
    float paintX_ = 0.0f;        // the last mouse x while painting velocities
    std::vector<NotePoint> lasso_;
    double lastLength_ = 1.0;    // new notes take the length and velocity of the last one edited
    float lastVelocity_ = 0.8f;
    int sounding_ = -1;          // the key being auditioned
    double soundUntil_ = -1.0;   // a key auditioned from the keyboard stops then
    double now_ = 0.0;
    double lastPress_ = -1.0;
    int lastPressNote_ = -1;
    int lastPressGhost_ = -1;
    float shownVelocity_ = -1.0f;
    std::string message_;
    ui::Hooks noteKeys_;
    static std::vector<Note> clipboard_;
};

}
