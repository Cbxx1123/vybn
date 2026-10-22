#pragma once

#include <cstdint>
#include <functional>

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
};

// The editor for the selected audio clip: its waveform on a ruler of bars, with the rest of its
// sound file dimmed on either side, so a trim can be undone by dragging back.
// - Drag either edge to trim the clip (the front keeps the audio where it was).
// - Drag the handles at the top corners to set the fade in and fade out.
// - Click to move the playhead; double-click to split the clip there.
// - The wheel scrolls in time; zoom() and fit() change the view.
// The waveform is drawn at the clip's gain.
class AudioEditorView : public ui::Component {
public:
    AudioEditorView(ProjectStore &store, AudioEditorHooks hooks);

    // The clip being edited, or null when the selection isn't an audio clip
    const Clip *clip() const;
    void zoom(float factor);
    // Shows the whole clip, at the next layout
    void fit() { fitPending_ = true; }

    ui::Vec2 measure(const ui::Theme &theme) const override;
    void arrange(const ui::Rect &rect, const ui::Theme &theme) override;
    void handleInput(const ui::Input &input) override;
    void draw(ui::DrawList &list, const ui::Theme &theme) const override;

private:
    const Project &project() const { return store_.project(); }

    enum class Drag { None, Start, End, FadeIn, FadeOut, Ruler };

    float beatX(double beat) const;
    double xBeat(float x) const;
    ui::Rect clipRect(const Clip &c) const;
    Drag hitTestHandles(const Clip &c, ui::Vec2 p) const;
    void fitClip(const Clip &c);
    // Everything drawContent reads, for the cache: the store's revisions, the clip shown and its
    // sound's address (null while loading), the layout, the view and the hover and drag states
    uint64_t contentKey(const Clip *c, const audio::AudioData *data) const;
    // The ruler, the file's waveform, the clip and its fades, the handles and the grabbed edges:
    // cached (see draw). The playhead is drawn after it, every frame.
    void drawContent(ui::DrawList &list, const ui::Theme &theme) const;

    ProjectStore &store_;
    AudioEditorHooks hooks_;
    ui::Rect ruler_, area_;
    float pixelsPerBeat_ = 40.0f;
    double viewStart_ = 0.0; // beat at the left edge
    bool fitPending_ = true;
    int shownTrack_ = -1, shownClip_ = -1;
    Drag drag_ = Drag::None;
    Drag hover_ = Drag::None;
    double lastPress_ = -1.0;
    ui::DrawCache cache_{"AudioEditorView"}; // drawContent, while nothing it reads has changed
};

}
