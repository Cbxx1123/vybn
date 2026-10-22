#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

#include "FoldState.h"
#include "Project.h"
#include "ProjectStore.h"
#include "Widgets.h"
#include "audio/AudioSystem.h"
#include "ui/UI.h"

namespace daw {

class AutomationWriter;

// A group's strip also has a chevron beside its name that folds it (hiding the strips of the tracks
// inside it, in the mixer and the arrangement). One track's channel strip: name, output (the master or a group: click for a menu of the groups
// that wouldn't loop), pan knob, fader with a meter, gain readout, a send bar for each return
// (lettered; right-click for pre or post fader, or to remove it), and mute, solo and (not on buses)
// arm. Pressing anywhere on it selects the track; the selected strip's name is in the
// accent colour. The meter shows the track's TrackProcessor peak, read once a frame into `levels`
// (empty while the track has no audio). The fader, pan knob and readout show what plays at the
// playhead (an automated volume or pan follows its lane); with a `writer`, moving them while the song
// plays and the track's writing is armed records them into the lanes (see AutomationWriter).
class ChannelStrip : public ui::Panel {
public:
    ChannelStrip(ui::Context &context, ProjectStore &store, int index, const std::vector<float> &levels,
                 AutomationWriter *writer = nullptr, FoldState *folds = nullptr);

    void handleInput(const ui::Input &input) override;

private:
    const Project &project() const { return store_.project(); }

    void openOutputMenu();

    ui::Context &context_;
    ProjectStore &store_;
    int index_;
    const std::vector<float> &levels_;
    AutomationWriter *writer_;
    ui::Label *name_ = nullptr;
    ui::Label *output_ = nullptr;
    ui::Knob *pan_ = nullptr;
    ui::LevelMeter *meter_ = nullptr;
};

// The sends into groups, by track id and group id (what the strips' send bars were made for; the mixer
// makes its strips again when this changes)
std::vector<uint64_t> groupSendShape(const Project &project);

// The master strip: the project's master volume (after the master effects; saved and undone), with
// the output's L and R meters, and the audio system's monitoring gain as a small "Out" knob (what
// you hear, never exported)
class MasterStrip : public ui::Panel {
public:
    MasterStrip(ProjectStore &store, audio::AudioSystem &audio, const StereoLevels &levels, AutomationWriter *writer = nullptr);

    void handleInput(const ui::Input &input) override;

private:
    const StereoLevels &levels_;
    audio::AudioParam &monitor_;
    ui::LevelMeter *meters_[2] = {};
    ui::Knob *out_ = nullptr;
};

// Channel strips side by side, scrolling across when there are many, with the master strip
// fixed at the right end and a button to add a track after the last strip
class MixerView : public ui::Container {
public:
    // `trackLevels` holds each track's peak this frame, by index
    // `onAddTrack` gets the kind the add button's menu picked (instrument, audio, return or group)
    // `writer` records the strips' volume, pan and sends into automation (none: never)
    MixerView(ui::Context &context, ProjectStore &store, audio::AudioSystem &audio, const StereoLevels &levels,
              const std::vector<float> &trackLevels, std::function<void(TrackKind)> onAddTrack,
              AutomationWriter *writer = nullptr, FoldState *folds = nullptr);

    ~MixerView() override;
    // The strips shown: the tracks not hidden inside a folded group
    size_t stripCount() const;

    // Rebuilds the strips from the project. handleInput posts it when the tracks change, so callers
    // don't need to; outside the tree's input and drawing it may also be called directly
    void rebuild();

    void handleInput(const ui::Input &input) override;

private:
    const Project &project() const { return store_.project(); }

    ui::Context &context_;
    ProjectStore &store_;
    const std::vector<float> &trackLevels_;
    std::function<void(TrackKind)> onAddTrack_;
    AutomationWriter *writer_;
    FoldState ownFolds_;               // used when none is shared
    FoldState *folds_;                 // how the strips fold groups (the project's Track::folded)
    uint64_t shownFold_ = 0;           // its version the strips were made for
    size_t stripCount_ = 0;
    ui::Container *strips_ = nullptr;
    uint64_t shownStructure_ = 0;      // the store's structureRevision last looked at
    uint64_t shownRevision_ = 0;
    std::vector<uint64_t> shownSends_; // the sends into groups the strips were made for
    std::vector<uint64_t> shownShape_; // the tracks (trackShape) the strips were made for
    // Cleared when the view is destroyed, so a rebuild posted before then does nothing
    std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
};

}
