#pragma once

#include <string>
#include <vector>

#include "DeviceControls.h"
#include "audio/SynthPresets.h"
#include "ui/UI.h"

namespace daw {

// The Vibin Synth's own panel, laid out like the front of a hardware synth: a preset bar over four
// rows of cards. Each card is a display of its part on top (the oscillator's wave, the sub and
// noise, the filter's response, the envelopes' shapes, the LFOs' waves) and its knobs below in one
// or two tight rows. Row 1 is Osc 1 and Osc 2; row 2 the filter and the two envelopes; row 3 the sub
// and noise and the two LFOs; row 4 the modulation matrix and the voice. The controls are the same
// ones the rest of the app uses (addParamControl), and every display reads the device's current
// values each frame, so presets, undo and the other editors all show through.
//
// Dragging on the displays edits the values: on the filter curve, horizontal moves the cutoff
// (log) and vertical the resonance, a double-click resets both; on the envelope, the attack's
// peak moves the attack, the corner moves the decay (x) and the sustain (y), and the release's
// end moves the release.
//
// Presets: the bar's dropdown shows the device's name, with "(edited)" when its values differ from
// the preset of that name (a name no preset has shows as Init when its values are the defaults).
// Previous and Next step through them, Init gives the defaults, and Save preset... writes the
// current values as a preset of the user's (SynthUserPresets). Choosing one is a single edit of the
// device's values and name.
class SynthView : public ui::Container {
public:
    SynthView(ui::Context &context, DeviceAccess access);

    void handleInput(const ui::Input &input) override;

private:
    // One entry of the preset dropdown; `special` is the device's own state when no preset shows it
    // as it is (its name edited, or a name no preset has)
    struct PresetEntry {
        std::string label;
        audio::SynthPreset preset;
        bool special = false;
    };

    // A card: its title, then its body, a column the displays and rows of controls go in
    ui::Container &card(ui::Container &parent, const std::string &title);
    void presetBar(ui::Container &parent);
    void oscillator(ui::Container &parent, int osc);
    void subAndNoise(ui::Container &parent);
    void filterCard(ui::Container &parent);
    void envelopeCard(ui::Container &parent, uint32_t base, const std::string &title, bool velocity);
    void lfoCard(ui::Container &parent, int lfo);
    void modulationCard(ui::Container &parent);
    void voiceCard(ui::Container &parent);

    // The LFO's rate column: its Rate, or its Sync rate when Sync is on (rebuilt when Sync changes)
    void rebuildRate(int lfo);

    std::string presetsSignature() const;
    void refreshPresets();
    int selectedPreset() const;
    void choosePreset(int index);
    void stepPreset(int delta);
    void initPreset();
    void openSaveDialog();

    ui::Context &context_;
    DeviceAccess access_;
    ui::Dropdown *presetDrop_ = nullptr;
    std::vector<PresetEntry> presets_; // the entries the dropdown shows, in the same order
    std::vector<audio::SynthPreset> user_;
    int userVersion_ = 0;
    std::string presetsBuilt_ = "?";
    bool presetsPosted_ = false;
    ui::Container *rate_[2] = {nullptr, nullptr};
    int rateBuilt_[2] = {-1, -1};
    bool ratePosted_[2] = {false, false};
};

}
