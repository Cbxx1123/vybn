#pragma once

#include <cstdint>
#include <functional>
#include <string>

#include "daw/model/Project.h"
#include "daw/model/ProjectStore.h"
#include "audio/AudioAnalysis.h"
#include "audio/AudioFile.h"
#include "ui/DrawCache.h"
#include "ui/UI.h"

namespace daw {

// What the audio editor reaches outside the project. Positions are in beats on the timeline.
struct AudioEditorHooks {
    // The sound a clip plays (loaded), or null while it loads
    std::function<const audio::AudioData *(const Clip &)> audioFor;
    // Its analysis (transients and tempo), or null while it is worked out
    std::function<const audio::AudioAnalysis *(const Clip &)> analysisFor;
    std::function<double()> position;
    std::function<void(double)> seek;
    // A warp marker's menu (right-click), by its index in the clip's markers
    std::function<void(int marker, ui::Vec2 at)> markerMenu;
};

// The editor for the selected audio clip: its waveform on a ruler of bars, with the rest of its
// sound file dimmed on either side, so a trim can be undone by dragging back.
// - Drag either edge to trim the clip (the front keeps the audio where it was).
// - Drag the handles at the top corners to set the fade in and fade out.
// - Click to move the playhead; double-click to split the clip there.
// - The wheel scrolls in time; zoom() and fit() change the view.
// The waveform is drawn at the clip's gain. A warped clip (docs/design/warp-plan.md) is drawn through
// its warp map, so it lines up with the bars, and has a strip of warp markers under the ruler:
// - Drag a marker to move it (snapped to the grid unless Shift): the sound either side stretches.
//   Alt-drag slips the sound under it instead.
// - Press the strip elsewhere to make a marker there (at the transient under the mouse, when there is
//   one) and drag it; double-click a marker to remove it; right-click one for its menu.
// - The transients found in the sound show as ticks along the waveform's top.
class AudioEditorView : public ui::Component {
public:
    AudioEditorView(ProjectStore &store, AudioEditorHooks hooks);

    // The clip being edited, or null when the selection isn't an audio clip
    const Clip *clip() const;
    void zoom(float factor);
    // Shows the whole clip, at the next layout
    void fit() { fitPending_ = true; }
    // What the mouse can do where it is, for the panel's header
    std::string hint() const;
    // The grid markers snap to, in beats (by zoom)
    double grid() const;

    ui::Vec2 measure(const ui::Theme &theme) const override;
    void arrange(const ui::Rect &rect, const ui::Theme &theme) override;
    void handleInput(const ui::Input &input) override;
    void draw(ui::DrawList &list, const ui::Theme &theme) const override;

    // For tests: where a marker's tab is drawn, the strip, and the x of a beat
    ui::Rect markerRect(int index) const;
    const ui::Rect &markerStrip() const { return strip_; }
    float xOfBeat(double beat) const { return beatX(beat); }

private:
    const Project &project() const { return store_.project(); }

    enum class Drag { None, Start, End, FadeIn, FadeOut, Ruler, Marker, Slip };

    float beatX(double beat) const;
    double xBeat(float x) const;
    ui::Rect clipRect(const Clip &c) const;
    Drag hitTestHandles(const Clip &c, ui::Vec2 p) const;
    void fitClip(const Clip &c);
    // The marker whose tab is under x in the strip, or -1
    int markerAt(const Clip &c, float x) const;
    // The beat a new marker pressed at x would go: the nearest transient within a few pixels, else x
    double ghostBeat(const Clip &c, float x) const;
    std::string readout() const;
    // Everything drawContent reads, for the cache: the store's revisions, the clip shown and its
    // sound's address (null while loading), the layout, the view and the hover and drag states
    uint64_t contentKey(const Clip *c, const audio::AudioData *data, const audio::AudioAnalysis *analysis) const;
    // The ruler, the file's waveform, the clip and its fades, the handles and the grabbed edges, the
    // markers and transients: cached (see draw). The playhead and the readout are drawn after it.
    void drawContent(ui::DrawList &list, const ui::Theme &theme) const;

    ProjectStore &store_;
    AudioEditorHooks hooks_;
    ui::Rect ruler_, strip_, area_;
    float pixelsPerBeat_ = 40.0f;
    double viewStart_ = 0.0; // beat at the left edge
    bool fitPending_ = true;
    int shownTrack_ = -1, shownClip_ = -1;
    Drag drag_ = Drag::None;
    Drag hover_ = Drag::None;
    int marker_ = -1;        // the marker being dragged
    int hoverMarker_ = -1;   // under the mouse in the strip
    double ghost_ = -1e9;    // where a press in the strip would make a marker (beats), -1e9 for none
    double slipFrom_ = 0.0;  // a slip's file seconds at the press, and the marker's
    double slipMarker_ = 0.0;
    Clip pressed_;           // the clip as the slip began
    ui::Vec2 mouse_;
    double lastPress_ = -1.0;
    ui::DrawCache cache_{"AudioEditorView"}; // drawContent, while nothing it reads has changed
};

}
