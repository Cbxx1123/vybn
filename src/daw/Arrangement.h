#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "Project.h"
#include "ProjectStore.h"
#include "audio/AudioFile.h"
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
};

// The controls at the start of one track's row: number, name, kind and input, and mute, solo and
// arm (R: record on this track). Pressing it elsewhere selects the track. The selected track's
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
    TrackHeader(ProjectStore &store, int index, const TransportHooks &hooks);

    TrackName &name() { return *title_; }

    void arrange(const ui::Rect &rect, const ui::Theme &theme) override;
    void handleInput(const ui::Input &input) override;
    void draw(ui::DrawList &list, const ui::Theme &theme) const override;

private:
    const Project &project() const { return store_.project(); }

    ProjectStore &store_;
    int index_;
    ui::Container *names_ = nullptr; // the name and caption, in a column
    TrackName *title_ = nullptr;   // the track's name
    ui::Label *midi_ = nullptr;    // "MIDI in" while the track plays incoming MIDI
    ui::Label *caption_ = nullptr; // its kind, or its input
    std::function<void(int track, ui::Rect line)> chooseInstrument_;
};

// The timeline: a ruler of bars over one lane per track, each with its clips, beside the track
// headers. A playhead follows the transport.
// - Press or drag on the ruler to move the playhead. Along its top, the loop strip: drag to set
//   the project's loop (by bars; Shift by beats), drag its ends, click in it to loop or not.
// - Press a clip to select it and drag it along its lane (snapped to beats) or onto another
//   track. Drag either end of a clip to trim it (snapped to beats; Shift for free); an audio
//   clip can't be stretched past its sound file. Double-click a clip to open it
//   (TransportHooks::openClip), or an empty instrument lane to add a one-bar clip there.
//   Instrument clips show their notes, audio clips their waveform (growing while recorded).
// - Right-click a clip or a track for its menu (TransportHooks::clipMenu, trackMenu).
// - The wheel scrolls the tracks; Shift or a sideways wheel scrolls in time, as does the bar
//   under the lanes. zoom() changes the scale.
// It follows the playhead while the transport plays.
class ArrangementView : public ui::Component {
public:
    ArrangementView(ProjectStore &store, TransportHooks transport);

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
    ui::Rect thumb() const;
    float contentWidth() const;
    void clampScroll();
    void drawClip(ui::DrawList &list, const ui::Theme &theme, int track, int index) const;

    ProjectStore &store_;
    TransportHooks transport_;
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
    bool draggingClip_ = false;
    int trimming_ = 0;     // -1 the start edge, 1 the end edge, while dragging it
    int hoverEdge_ = 0;    // the edge under the mouse, likewise
    bool draggingBar_ = false;
    double grabBeat_ = 0.0;  // mouse beat minus clip start, while dragging a clip
    float grabX_ = 0.0f;     // mouse offset into the thumb, while dragging the bar
    int hoveredTrack_ = -1;
    int dropRow_ = -1;
    std::string dropHint_;
    int hoveredClip_ = -1;
    double lastPress_ = -1.0;
};

}
