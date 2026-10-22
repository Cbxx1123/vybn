#pragma once

#include <cstdint>
#include <functional>

#include "daw/model/ProjectStore.h"
#include "ui/UI.h"

namespace daw {

// What the lanes ask of their view and the app: where a beat is across the lanes and back, the
// view's grid, the playhead, and menus and editors (windows the view doesn't own). All must be set
// but the menus and editors.
struct TimingHooks {
    std::function<float(double beat)> beatX;
    std::function<double(float x)> xBeat;
    std::function<double(double beat, bool free)> snap;
    std::function<double()> position;
    std::function<void(double beat)> seek;                          // the playhead to a beat
    std::function<void(int point, ui::Vec2 at)> tempoMenu;          // a tempo point's menu
    std::function<void(int signature, ui::Vec2 at)> signatureMenu;  // a signature change's menu
    std::function<void(int point)> editTempo;                       // a point's tempo, typed
    std::function<void(int signature)> editSignature;               // a change's numerator and unit
    // Empty lane space right-clicked at `beat` (`tempo`: in the tempo lane, else the signature lane)
    std::function<void(ui::Vec2 at, double beat, bool tempo)> laneMenu;
    // Markers: a marker's name to edit, and a marker's menu (kNoId: empty space at `beat`)
    std::function<void(ObjectId marker)> renameMarker;
    std::function<void(ObjectId marker, double beat, ui::Vec2 at)> markerMenu;
};

// The song's markers and tempo map as three lanes across the top of the arrangement, under its ruler,
// each with a header in the header column. ArrangementView owns it, lays it out, and passes it input
// first.
// - Markers: a flag per marker with its name. Click one to put the playhead there, drag it to move it
//   (snapped unless Shift), double-click it to rename it, right-click for its menu. Double-click
//   empty space to add one there (then name it). The header shows the section the playhead is in.
// - Signature: a marker at each change ("7/8"). Hovering a bar shows where a change would go;
//   double-click there to add one (the signature in force, then its editor opens), double-click a
//   marker to edit it, drag one along the bars, right-click for a menu (common signatures, delete).
//   The first, at bar 1, can't move or go.
// - Tempo: the tempo over the song, held or ramping between its points, filled underneath.
//   Hovering shows where a click would add a point and its tempo; click to add one (snapped to the
//   grid unless Shift is held; BPM in whole numbers, tenths with Shift), drag a point between its
//   neighbours (the first only up and down), double-click one to type its tempo, right-click for
//   its menu (ramp to the next, delete).
// - A click selects a point or a marker (ringed); Delete removes it (not the first). A press
//   anywhere else lets go of it. Right-clicking empty lane space opens the lane's menu.
// The headers show the signature and the tempo at the playhead and the tempo lane's scale.
class TimingLanes {
public:
    static constexpr float kMarkerHeight = 24.0f;
    static constexpr float kSignatureHeight = 26.0f;
    static constexpr float kTempoHeight = 64.0f;
    static constexpr float kGap = 4.0f;
    // The height of the lanes and the gaps under them
    static constexpr float height() { return kMarkerHeight + kSignatureHeight + kTempoHeight + 3.0f * kGap; }

    TimingLanes(ProjectStore &store, TimingHooks hooks);

    // Where it goes: the header column's part and the lanes' part (both height() tall)
    void arrange(const ui::Rect &headers, const ui::Rect &lanes);
    // Takes the input over the lanes and their headers; true when it did (a press there, a drag)
    bool handleInput(const ui::Input &input);
    // The lanes (cached by the arrangement: what they show moves key() or the store's revision) and
    // the headers (every frame: they show the playhead's tempo)
    void drawLanes(ui::DrawList &list, const ui::Theme &theme) const;
    void drawHeaders(ui::DrawList &list, const ui::Theme &theme) const;
    uint64_t key() const;

    // The lanes' BPM scale: the points' range, padded to tens, at least 40 BPM tall
    void tempoRange(double &low, double &high) const;
    // The selected tempo point and signature change (-1 for none), and removing what is selected
    // (returns false when nothing removable is)
    int selectedPoint() const;
    int selectedMarker() const;
    ObjectId selectedFlag() const { return selectedFlag_; } // the song marker selected (kNoId for none)
    bool deleteSelected();

private:
    const Project &project() const { return store_.project(); }
    float beatX(double beat) const { return hooks_.beatX(beat); }
    double xBeat(float x) const { return hooks_.xBeat(x); }
    float bpmY(double bpm) const;
    double yBpm(float y, bool fine) const;
    int pointAt(ui::Vec2 p) const;  // the tempo point under p, or -1
    int markerAt(ui::Vec2 p) const; // the signature change under p, or -1
    ui::Rect markerRect(const audio::Signature &s) const;
    void edit(const audio::TempoMap &timing) { store_.setTiming(timing); }

    ProjectStore &store_;
    TimingHooks hooks_;
    ui::Hooks keys_;
    ui::Rect markerHeader_, markerLane_, signatureHeader_, signatureLane_, tempoHeader_, tempoLane_;
    // Song markers: the flag under a point (kNoId for none), from where the flags were last drawn
    ObjectId flagAt(ui::Vec2 p) const;
    mutable std::vector<std::pair<ObjectId, ui::Rect>> flagRects_;
    ObjectId hoverFlag_ = kNoId;
    ObjectId dragFlag_ = kNoId;     // the flag pressed (moved once dragged past a few pixels)
    float flagPressX_ = 0.0f;
    bool flagMoved_ = false;
    ObjectId selectedFlag_ = kNoId;
    bool hoverFlags_ = false;       // the mouse over the markers lane
    double ghostFlagBeat_ = -1.0;   // where a double-click would add a marker (-1 for nowhere)

    int hoverPoint_ = -1;
    int hoverMarker_ = -1;
    bool hoverTempo_ = false;
    bool hoverSignature_ = false;
    double ghostBeat_ = -1.0;  // where a click in the tempo lane would add a point (-1 for nowhere)
    double ghostBpm_ = 0.0;
    int ghostBar_ = -1;        // the bar a double-click in the signature lane would add a change at
    int dragPoint_ = -1;
    int dragMarker_ = -1;
    double dragLow_ = 0.0, dragHigh_ = 0.0; // the scale while a point is dragged, so it holds still
    // The selection, by place (a beat, a bar), so it follows its point through edits
    double selectedBeat_ = -1.0;
    int selectedBar_ = -1;
    double lastPress_ = -1.0;
    int lastPressed_ = -2; // what the last press was on (a point, a marker or -1 for empty), for double-clicks
    int lastPressLane_ = -1; // which lane the last press was in: 0 markers, 1 signatures, 2 tempo
};

}
