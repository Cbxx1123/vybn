#pragma once

#include <functional>
#include <string>
#include <vector>

#include "DeviceCatalogue.h"
#include "DeviceControls.h"
#include "ProjectStore.h"
#include "Project.h"
#include "Widgets.h"
#include "audio/Device.h"
#include "ui/UI.h"

namespace daw {

// What the devices panel reaches through the app
struct DevicesHooks {
    // The audio side's device at a place (nullptr until the chain has made it)
    std::function<audio::Device *(const DevicePlace &place)> device;
    // A device's full view: every parameter, and a plugin's own interface
    std::function<void(const DevicePlace &place)> openDevice;
    // The device picker: an instrument for `track`, or an effect added to the master's chain or
    // the track's, opened beside `anchor`
    std::function<void(bool instrument, bool master, int track, ui::Rect anchor)> pick;
    // A track's plugin latency and the delay compensation it gets, as words (empty: none)
    std::function<std::string(int track)> latencyText;
    // A library instrument's state: loading, its memory, or why it isn't playing
    std::function<std::string(int track)> instrumentStatus;
};

// The Devices panel: the chain being edited as a row of cards. For a track, its instrument comes
// first (or an empty place to choose one), then its effects, then a + that adds one; the master
// has effects only. Every card is built the same way whatever the device: its name and kind, its
// first few parameters as knobs (switches and choices where that's what they are), bypass (an
// effect), and buttons to open its full view, move it, change it (the instrument) or remove it.
// A plugin's card says when it is loading or why it isn't playing, with Reload. It edits the
// selected track's chain or the master's (setMaster); the settings live in the Project, so undo,
// saving and the audio thread's chain all follow. The cards are rebuilt after the frame whenever
// the target or its devices change.
class DevicesView : public ui::Container {
public:
    DevicesView(ui::Context &context, ProjectStore &store, DeviceCatalogue &catalogue, DevicesHooks hooks);

    void setMaster(bool master) { master_ = master; }
    bool master() const { return master_; }
    // "Master", the selected track's name (with its latency), or a hint when nothing is selected
    std::string targetName() const;

    void handleInput(const ui::Input &input) override;

    // The most knobs a card shows; its full view has the rest
    static constexpr size_t kCardParams = 8;

private:
    std::string signature();
    void rebuild();
    DeviceAccess access(const DevicePlace &place);
    // The chain (the master's or a track's) to change: only after its existence is known
    std::vector<audio::DeviceSetting> &editChain(bool master, int track);
    const Project &project() const { return store_.project(); }
    void buildCard(ui::Container &row, const DevicePlace &place);
    void buildEmptyInstrument(ui::Container &row);
    void buildBody(ui::Container &card, const DevicePlace &place);
    std::string sourceText(const audio::DeviceSetting &setting, bool instrument);
    void pickEffect(ui::Rect anchor);

    ui::Context &context_;
    ProjectStore &store_;
    DeviceCatalogue &catalogue_;
    DevicesHooks hooks_;
    ui::Container *cards_ = nullptr;
    bool master_ = false;
    std::string built_ = "?"; // what the cards were built for
    bool rebuilding_ = false;
};

}
