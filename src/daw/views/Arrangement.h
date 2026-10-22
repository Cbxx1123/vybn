#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "daw/model/AutomationLanes.h"
#include "daw/model/FoldState.h"
#include "daw/model/Project.h"
#include "daw/model/ProjectStore.h"
#include "daw/views/TimingLanes.h"
#include "daw/widgets/Widgets.h"
#include "audio/AudioFile.h"
#include "ui/DrawCache.h"
#include "ui/UI.h"

namespace daw {

// How the arrangement reaches the transport, which it doesn't own. Positions are in beats.
struct TransportHooks {
    std::function<double()> position;
    std::function<bool()> playing;
    std::function<void(double)> seek;
    // A clip was double-clicked (it's selected first): open it in an editor
    std::function<void()> openClip;
    // The sound an audio clip plays (or its take, while recording), or null
    std::function<const audio::AudioData *(int track, int clip)> audioFor;
    std::function<bool(int track, int clip)> isRecording;
    // What a track's header says under its name: its kind and input
    std::function<std::string(int track)> trackInput;
    // Whether a track is playing the MIDI it's sent (shown as "MIDI in" above its kind)
    std::function<bool(int track)> trackListening;
    // Right-clicks: on a clip (selected first), at a beat; on a track's header or empty lane
    std::function<void(ui::Vec2 at, double beat)> clipMenu;
    std::function<void(int track, ui::Vec2 at)> trackMenu;
    // An instrument track's instrument line was clicked (at `line`): choose its instrument
    std::function<void(int track, ui::Rect line)> chooseInstrument;
    // Automation: a device's parameters as it has them now (a plugin's once it has loaded), for the
    // lanes' names and values; and the menu of what a track can automate, opened at `at`
    std::function<std::vector<audio::DeviceParam>(const DevicePlace &place)> deviceParams;
    std::function<void(int track, ui::Vec2 at)> automationMenu;
    // Recording automation: whether a track's writing is armed, and arming it (its header's W
    // toggle, shown only when both are set; see AutomationWriter)
    std::function<bool(int track)> automationArmed;
    std::function<void(int track, bool on)> armAutomation;
    // The same for the master row: the menu of what the master can automate, opened at `at`, and its
    // writing (the master header's W toggle, shown only when both are set)
    std::function<void(ui::Vec2 at)> masterAutomationMenu;
    std::function<bool()> masterAutomationArmed;
    std::function<void(bool on)> armMasterAutomation;
    // The tempo and signature lanes (TimingLanes): a tempo point's menu and a signature change's,
    // opened at `at`, their editors, and the menu of empty lane space at `beat`
    std::function<void(int point, ui::Vec2 at)> tempoMenu;
    std::function<void(int signature, ui::Vec2 at)> signatureMenu;
    std::function<void(int point)> editTempo;
    std::function<void(int signature)> editSignature;
    std::function<void(ui::Vec2 at, double beat, bool tempo)> timingLaneMenu;
    // The lanes were shown or hidden (to remember it)
    std::function<void(bool shown)> timingShown;
};

// The controls at the start of one track's row: number, name, kind and input, and a disc of its
// toggles (mute, solo, record arm, automation shown, automation written) that opens them as a
// radial menu (RadialToggles). Pressing it elsewhere selects the track. The selected track's
// header is pressed in.
// A track's name in its header, renamed in place: double-click it (or call beginEdit) and type;
// Enter, Escape or a click elsewhere finishes. The name changes as it is typed; left empty it
// goes back to what it was.
class TrackName : public ui::Component {
public:
    TrackName(ProjectStore &store, int index);

    void setMaxWidth(float width);
    void beginEdit();
    void endEdit();
    bool editing() const { return editing_; }

    ui::Vec2 measure(const ui::Theme &theme) const override;
    void arrange(const ui::Rect &rect, const ui::Theme &theme) override;
    void handleInput(const ui::Input &input) override;
    void draw(ui::DrawList &list, const ui::Theme &theme) const override;
    // While editing, the field (which may be wider than the name)
    bool hitTest(ui::Vec2 p) const override { return editing_ ? field_->rect().contains(p) : rect_.contains(p); }

private:
    const Project &project() const { return store_.project(); }

    ProjectStore &store_;
    int index_;
    std::unique_ptr<ui::Label> label_;
    std::unique_ptr<ui::TextField> field_;
    float maxWidth_ = 0.0f;
    bool editing_ = false;
    bool wantFocus_ = false; // focus the field once it has had input
    bool hadFocus_ = false;
    double lastPress_ = -1.0;
    std::string before_; // the name when editing began
};

class TrackHeader : public ui::Container {
public:
    // `automationShown` and `showAutomation` back its A toggle: the track's automation lanes shown
    // `folded` and `fold` back a group's chevron (shown instead of its number) and `foldedCount` its
    // "N tracks folded" caption (see ArrangementView::fold); they may be empty, as for any other track
    TrackHeader(ProjectStore &store, int index, const TransportHooks &hooks, std::function<bool()> automationShown,
                std::function<void(bool)> showAutomation, std::function<bool()> folded = {},
                std::function<void(bool)> fold = {}, std::function<int()> foldedCount = {});

    TrackName &name() { return *title_; }
    // Whether a press at `p` starts a drag of the track (not on its toggles, the name being typed,
    // or an instrument's line, which chooses the instrument)
    bool canDrag(ui::Vec2 p) const;
    // Where the group's chevron is (an empty rect for any other track)
    ui::Rect chevronRect() const;

    void arrange(const ui::Rect &rect, const ui::Theme &theme) override;
    void handleInput(const ui::Input &input) override;
    void draw(ui::DrawList &list, const ui::Theme &theme) const override;

private:
    const Project &project() const { return store_.project(); }

    ProjectStore &store_;
    int index_;
    ui::Container *names_ = nullptr; // the name and caption, in a column
    TrackName *title_ = nullptr;   // the track's name
    ui::Component *chevron_ = nullptr; // a group's fold control, in the number's place
    ui::Label *midi_ = nullptr;    // "MIDI in" while the track plays incoming MIDI
    ui::Label *caption_ = nullptr; // its kind, or its input
    RadialToggles *flags_ = nullptr; // M, S, R, A (and W) as a radial menu
    std::function<void(int track, ui::Rect line)> chooseInstrument_;
};

// The master's row header: its name and a disc with the A toggle (its automation lanes shown) and,
// when the app records automation, W. It has no clips, mute, solo or arm, and isn't dragged.
class MasterHeader : public ui::Container {
public:
    MasterHeader(const TransportHooks &hooks, std::function<bool()> automationShown, std::function<void(bool)> showAutomation);

private:
    ui::Container *names_ = nullptr;
};

// The timeline: a ruler of bars, the song's tempo and time signatures (TimingLanes, which can be
// hidden), and one lane per track, each with its clips, beside the track headers. A playhead
// follows the transport.
// - Press or drag on the ruler to move the playhead. Along its top, the loop strip: drag to set
//   the project's loop (by bars; Shift by beats), drag its ends, click in it to loop or not.
// - Press a clip to select it and drag it along its lane (snapped to beats) or onto another
//   track. Drag either end of a clip to trim it (snapped to beats; Shift for free); an audio
//   clip can't be stretched past its sound file. Double-click a clip to open it
//   (TransportHooks::openClip), or an empty instrument lane to add a one-bar clip there.
//   Instrument clips show their notes, audio clips their waveform (growing while recorded).
// - Drag a track's header up or down to move the track: it follows the mouse as a preview, and the
//   other tracks make room for it where it would land.
// - Right-click a clip or a track for its menu (TransportHooks::clipMenu, trackMenu).
// - A track's A toggle shows its automation: a lane under its row for each parameter it automates
//   (its volume and pan, its devices' parameters), and a strip to add another
//   (TransportHooks::automationMenu). In a lane, click to add a point (snapped to the grid unless
//   Shift is held) and drag points to move them, between their neighbours; double-click or
//   right-click a point to remove it. A lane's header shows its value at the playhead, and its x
//   removes the lane.
// - Under the last track is the master's row: a header (its A toggle shows the master's lanes: its
//   volume and its effects' parameters, edited like a track's; TransportHooks::masterAutomationMenu)
//   and no clips. It is never a track: rowAt, drops, reordering and clip dragging skip it.
// - Under the ruler, the tempo and signature lanes (TimingLanes). Hidden, the ruler marks each
//   signature change ("7/8" by its bar's number) and each tempo change (a dot).
// - The wheel scrolls the tracks; Shift or a sideways wheel scrolls in time, as does the bar
//   under the lanes. zoom() changes the scale.
// It follows the playhead while the transport plays.
class ArrangementView : public ui::Component {
public:
    // `folds` is the fold state shared with the mixer; without one the view keeps its own
    ArrangementView(ProjectStore &store, TransportHooks transport, FoldState *folds = nullptr);

    // Rebuilds the track headers from the project. handleInput calls it when the tracks change, so
    // callers don't need to; it may also be called directly outside the tree's input and drawing
    void rebuild();
    void zoom(float factor);
    void setFollow(bool follow) { follow_ = follow; }
    // Starts renaming a track in its header
    void renameTrack(int index);
    // Dropping something on the timeline: the track at `p` (in the header column or the lanes),
    // the number of tracks for the empty space under the last one, or -1 (outside, or the ruler)
    int trackAt(ui::Vec2 p) const;
    // Highlights where a drop would go (-1 for nowhere), with a hint in the empty space
    void setDropTarget(int row, std::string hint = {});
    bool follow() const { return follow_; }
    // Whether a track's automation lanes are shown (kept by the track's id, not saved), and showing them
    bool automationShown(int track) const;
    void showAutomation(int track, bool shown);
    // Whether a group track is folded (its tracks hidden: those that go into it, directly or through
    // groups inside it), and folding it. Kept by the group's id, not saved or undone, like the lanes
    // shown. False for any track but a group.
    bool folded(int track) const;
    void fold(int track, bool folded);
    // Whether a track is hidden by a folded group: it takes no height, and is never found at a point
    bool hidden(int track) const;
    // Folds every group, or unfolds them all
    void foldAll();
    void unfoldAll();
    // How many groups deep a track's header is indented (0 for the master)
    int depth(int track) const;
    // The tracks folded away inside a group while it is folded (0 otherwise)
    int foldedCount(int track) const;
    // The header of a track, or null when it has none (hidden, or no such track)
    const TrackHeader *header(int track) const;
    // The same for the master row's lanes
    bool masterAutomationShown() const { return openMaster_; }
    void showMasterAutomation(bool shown);
    // Whether the tempo and signature lanes are shown (a view setting, kept by the app through
    // TransportHooks::timingShown). The chip in the corner left of the ruler, which shows the tempo
    // and signature at the playhead, shows and hides them too.
    bool timingShown() const { return timingShown_; }
    void showTiming(bool shown);
    const TimingLanes &timingLanes() const { return *timing_; }

    ui::Vec2 measure(const ui::Theme &theme) const override;
    void arrange(const ui::Rect &rect, const ui::Theme &theme) override;
    void handleInput(const ui::Input &input) override;
    void draw(ui::DrawList &list, const ui::Theme &theme) const override;

private:
    const Project &project() const { return store_.project(); }

    float beatX(double beat) const;
    double xBeat(float x) const;
    int rowAt(float y) const; // track index under a y coordinate, or -1
    ui::Rect clipRect(int track, const Clip &clip) const;

    // The rows: each track's own (kTrackHeight), with its automation's lanes and the strip to add
    // one under it while shown. Worked out from the project every arrange.
    struct Row {
        float top = 0.0f;    // from the top of the lanes, unscrolled
        float height = 0.0f;
        bool open = false;   // its automation shown
        std::vector<AutomationRow> lanes;
    };
    void layoutRows();
    float rowTop(int track) const;    // on screen
    float rowHeight(int track) const;
    float rowsHeight() const;          // all of them, the master's row last
    // The master's row has the index after the last track in the lane functions below
    bool isMaster(int track) const { return track == static_cast<int>(project().tracks.size()); }
    const Row *rowOf(int track) const; // a track's row, or the master's; null for neither
    float headHeight(int track) const; // the height of a row's own part, above its lanes
    int laneRowAt(float y) const;      // rowAt, or the master's index on its row
    // A shown lane in the lanes, its header in the header column, and the strip to add one
    ui::Rect laneRect(int track, size_t lane) const;
    ui::Rect laneHeaderRect(int track, size_t lane) const;
    ui::Rect laneRemoveRect(int track, size_t lane) const;
    ui::Rect addLaneRect(int track) const;
    // A lane's points: their y for a position, and the position for a y
    float laneY(const ui::Rect &lane, float position) const;
    float lanePosition(const ui::Rect &lane, float y) const;
    double laneSnap(double beat, bool free) const;
    // A lane's parameter (its name and range), and its value now (the project's own, for a lane
    // with no points)
    audio::DeviceParam laneParam(int track, const AutomationRow &row) const;
    std::string laneName(int track, const AutomationRow &row) const;
    float restingPosition(int track, const AutomationRow &row) const;
    const audio::AutomationLane *laneOf(int track, const AutomationRow &row) const;
    void drawLanes(ui::DrawList &list, const ui::Theme &theme) const;
    void drawLaneHeaders(ui::DrawList &list, const ui::Theme &theme) const;
    // Handles a press, drag or hover in the lanes and their headers; true when it took the input
    bool handleLaneInput(const ui::Input &input);
    ui::Rect thumb() const;
    float contentWidth() const;
    void clampScroll();
    void drawClip(ui::DrawList &list, const ui::Theme &theme, int track, int index) const;
    // Whether a clip's rect (from clipRect) is in the lanes at all: drawClip draws nothing else
    bool clipShown(const ui::Rect &r) const;
    // Whether any clip is being recorded (its take grows every frame)
    bool anyRecording() const;
    // Everything drawContent reads, for the cache; `recording` is anyRecording()
    uint64_t contentKey(bool recording) const;
    // The ruler, the lanes with their clips, and the scroll bar: cached (see draw)
    void drawContent(ui::DrawList &list, const ui::Theme &theme) const;

    ProjectStore &store_;
    TransportHooks transport_;
    ui::DrawCache cache_{"ArrangementView"}; // drawContent, while nothing it reads has changed
    std::vector<std::unique_ptr<TrackHeader>> headers_; // not tree children, so rebuilt in handleInput
    uint64_t shownStructure_ = 0;                       // the store's structureRevision last looked at
    std::vector<uint64_t> shownShape_;                  // the tracks (trackShape) the headers were made for
    float pixelsPerBeat_ = 22.0f;
    float scrollX_ = 0.0f;
    float scrollY_ = 0.0f;
    // Middle-drag panning: the mouse and scroll where it began
    bool panning_ = false;
    ui::Vec2 panMouse_;
    float panX_ = 0.0f, panY_ = 0.0f;
    bool follow_ = true;

    ui::Rect ruler_, headerArea_, lanes_, well_, bar_;

    // Interaction
    bool scrubbing_ = false;
    // The loop strip: 0 not dragging it, 1 a new range, 2 its start, 3 its end
    int loopDrag_ = 0;
    double loopPress_ = 0.0;
    float loopPressX_ = 0.0f;
    bool loopMoved_ = false;
    ui::Rect loopBand() const;
    double loopSnap(double beat, bool shift) const;
    // Reordering: the track whose header is dragged (-1 for none), the row it would go to, the mouse
    // y at the press and whether it has moved far enough to count as a drag
    int reorderFrom_ = -1;
    std::vector<int> reorderBlock_; // what moves with it: itself, or a folded group and the tracks hidden in it
    int reorderTo_ = -1;
    float reorderPressY_ = 0.0f;
    float reorderGrab_ = 0.0f;   // the press's offset into the dragged header
    float reorderMouseY_ = 0.0f; // the mouse y now, while dragging
    bool reorderMoved_ = false;
    bool draggingClip_ = false;
    int trimming_ = 0;    // -1 the start edge, 1 the end edge, while dragging it
    int hoverEdge_ = 0;    // the edge under the mouse, likewise
    bool draggingBar_ = false;
    double grabBeat_ = 0.0;  // mouse beat minus clip start, while dragging a clip
    float grabX_ = 0.0f;     // mouse offset into the thumb, while dragging the bar
    int hoveredTrack_ = -1;
    int dropRow_ = -1;
    std::string dropHint_;
    int hoveredClip_ = -1;
    double lastPress_ = -1.0;

    // Automation
    std::vector<Row> rows_;
    Row master_;                        // the master's row, after the tracks
    bool openMaster_ = false;           // its lanes shown
    std::unique_ptr<MasterHeader> masterHeader_;
    std::set<ObjectId> openAutomation_; // the tracks whose lanes are shown
    FoldState ownFolds_;                // used when none is shared
    FoldState *folds_;                  // the groups folded
    std::vector<char> hidden_;          // by track index, from folds_ (worked out in layoutRows)
    std::vector<int> depth_;            //   how many groups deep each track goes
    std::vector<int> foldedCounts_;     //   for a folded group, the tracks hidden inside it
    uint64_t foldedFor_ = 0;            // the store's revision and the fold version hidden_, depth_ and
    uint64_t foldedVersion_ = 0;        //   foldedCounts_ were worked out for
    int laneTrack_ = -1;                // the lane under the mouse (or being dragged in): its track
    int laneIndex_ = -1;                //   and its index among the track's shown lanes
    int lanePoint_ = -1;                // the point under the mouse (or being dragged)
    bool draggingPoint_ = false;
    bool pointMoved_ = false;
    int hoveredRemove_ = -1;            // a lane's remove button under the mouse (its index), on laneTrack_
    int hoveredAdd_ = -1;               // the track whose add strip is under the mouse
    double lastPointPress_ = -1.0;
    int lastPressedPoint_ = -1;
    bool lanePress_ = false; // this frame's press was in the lanes or their headers (handleLaneInput took it)

    // The tempo and signature lanes under the ruler
    std::unique_ptr<TimingLanes> timing_;
    bool timingShown_ = true;
    ui::Rect timingChip() const; // the corner chip
    bool chipHovered_ = false;
};

}
