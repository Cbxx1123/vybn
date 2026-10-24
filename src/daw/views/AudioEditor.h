#pragma once

#include <cstdint>
#include <functional>

#include "daw/audio/AudioPool.h"
#include "daw/model/Project.h"
#include "daw/model/ProjectStore.h"
#include "audio/AudioFile.h"
#include "ui/DrawCache.h"
#include "ui/UI.h"

namespace daw {

// What the audio editor reaches outside the project. Positions are in beats on the timeline.
struct AudioEditorHooks {
    // The sound a clip plays (loaded), or null while it loads
    std::function<const audio::AudioData *(const Clip &)> audioFor;
    std::function<double()> position;
    std::function<void(double)> seek;
    // The clip's file's transients (null while they are found), and a number that moves when they
    // or a stretched sound arrive (the pool's version)
    std::function<const FileOnsets *(const Clip &)> onsets;
    std::function<uint64_t()> version;
};

// The editor for the selected audio clip: its waveform on a ruler of bars, with the rest of its
// sound file dimmed on either side, so a trim can be undone by dragging back.
// - Drag either edge to trim the clip (the front keeps the audio where it was).
// - Drag the handles at the top corners to set the fade in and fade out.
// - Click to move the playhead; double-click to split the clip there.
// - The wheel scrolls in time; zoom() and fit() change the view.
// - A warped clip (docs/design/warping-plan.md) has a warp strip under the ruler: its file's transients
//   as ticks and its warp markers as pins. Double-click the strip to add a marker (on a transient
//   within a few pixels), drag one to move its beat (snapped to sixteenths; Shift for free), and
//   double-click one to delete it. The waveform is drawn through the warp.
// The waveform is drawn at the clip's gain.
class AudioEditorView : public ui::Component {
public:
    AudioEditorView(ProjectStore &store, AudioEditorHooks hooks);

    // The clip being edited, or null when the selection isn't an audio clip
    const Clip *clip() const;
    void zoom(float factor);
    // Shows the whole clip, at the next layout
    void fit() { fitPending_ = true; }
    // What the mouse can do where it is, for the editor's header (empty when nothing special)
    std::string hint() const;

    ui::Vec2 measure(const ui::Theme &theme) const override;
    void arrange(const ui::Rect &rect, const ui::Theme &theme) override;
    void handleInput(const ui::Input &input) override;
    void draw(ui::DrawList &list, const ui::Theme &theme) const override;

private:
    const Project &project() const { return store_.project(); }

    enum class Drag { None, Start, End, FadeIn, FadeOut, Ruler, Marker };

    float beatX(double beat) const;
    double xBeat(float x) const;
    ui::Rect clipRect(const Clip &c) const;
    Drag hitTestHandles(const Clip &c, ui::Vec2 p) const;
    // The warp marker under a point in the strip (-1 for none), and the transient nearest a beat
    // within `pixels` (-1 for none)
    int markerAt(const Clip &c, ui::Vec2 p) const;
    int transientNear(const Clip &c, double beat, float pixels) const;
    // Double-clicks and drags in the warp strip
    void pressStrip(const Clip &c, ui::Vec2 p, bool doubleClick);
    void dragMarker(const ui::Input &input);
    void fitClip(const Clip &c);
    // Everything drawContent reads, for the cache: the store's revisions, the clip shown and its
    // sound's address (null while loading), the layout, the view and the hover and drag states
    uint64_t contentKey(const Clip *c, const audio::AudioData *data) const;
    // The ruler, the file's waveform, the clip and its fades, the handles and the grabbed edges:
    // cached (see draw). The playhead is drawn after it, every frame.
    void drawContent(ui::DrawList &list, const ui::Theme &theme) const;

    ProjectStore &store_;
    AudioEditorHooks hooks_;
    ui::Rect ruler_, strip_, area_; // strip_ is empty unless the clip is warped
    float pixelsPerBeat_ = 40.0f;
    double viewStart_ = 0.0; // beat at the left edge
    bool fitPending_ = true;
    int shownTrack_ = -1, shownClip_ = -1;
    Drag drag_ = Drag::None;
    Drag hover_ = Drag::None;
    double lastPress_ = -1.0;
    int hoverMarker_ = -1, dragMarker_ = -1;
    double markerAnchor_ = 0.0; // the clip's anchor (warpAnchor) when the marker drag began
    ui::DrawCache cache_{"AudioEditorView"}; // drawContent, while nothing it reads has changed
};

}
