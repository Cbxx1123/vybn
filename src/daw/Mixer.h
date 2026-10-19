#pragma once

#include <functional>
#include <vector>

#include "Project.h"
#include "ProjectStore.h"
#include "Widgets.h"
#include "audio/AudioSystem.h"
#include "ui/UI.h"

namespace daw {

// One track's channel strip: name, pan knob, fader with a meter, gain readout, and mute, solo
// and arm. Pressing anywhere on it selects the track; the selected strip's name is in the
// accent colour. The meter shows the track's TrackProcessor peak, read once a frame into `levels`
// (empty while the track has no audio).
class ChannelStrip : public ui::Panel {
public:
    ChannelStrip(ProjectStore &store, int index, const std::vector<float> &levels);

    void handleInput(const ui::Input &input) override;

private:
    const Project &project() const { return store_.project(); }

    ProjectStore &store_;
    int index_;
    const std::vector<float> &levels_;
    ui::Label *name_ = nullptr;
    ui::Knob *pan_ = nullptr;
    ui::LevelMeter *meter_ = nullptr;
};

// The master strip: the audio system's real master gain, with the output's L and R meters
class MasterStrip : public ui::Panel {
public:
    MasterStrip(audio::AudioSystem &audio, const StereoLevels &levels);

    void handleInput(const ui::Input &input) override;

private:
    const StereoLevels &levels_;
    ui::LevelMeter *meters_[2] = {};
};

// Channel strips side by side, scrolling across when there are many, with the master strip
// fixed at the right end and a button to add a track after the last strip
class MixerView : public ui::Container {
public:
    // `trackLevels` holds each track's peak this frame, by index
    // `onAddTrack` gets the kind the add button's menu picked (instrument or audio)
    MixerView(ui::Context &context, ProjectStore &store, audio::AudioSystem &audio, const StereoLevels &levels,
              const std::vector<float> &trackLevels, std::function<void(TrackKind)> onAddTrack);

    // Rebuilds the strips from the project; call outside the tree's input and drawing
    void rebuild();

private:
    const Project &project() const { return store_.project(); }

    ui::Context &context_;
    ProjectStore &store_;
    const std::vector<float> &trackLevels_;
    std::function<void(TrackKind)> onAddTrack_;
    ui::Container *strips_ = nullptr;
};

}
