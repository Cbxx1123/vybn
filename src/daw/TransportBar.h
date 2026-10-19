#pragma once

#include "ProjectStore.h"
#include "Widgets.h"
#include "audio/AudioSystem.h"
#include "audio/Processors.h"
#include "ui/UI.h"

namespace daw {

// Positions in beats from the transport's samples, at the running device's rate
double transportBeats(audio::AudioSystem &audio);
void seekBeats(audio::AudioSystem &audio, double beats);
// The transport's loop range from beats, looping when `on`
void setLoopBeats(audio::AudioSystem &audio, double start, double end, bool on);

// Recording, and the inputs shown on the transport bar
struct RecordHooks {
    std::function<bool()> recording;
    std::function<void()> toggle;
    std::function<std::string()> midiInput;  // the MIDI devices in use ("" for none)
    std::function<bool()> midiActive;         // a message arrived just now
    std::function<std::string()> audioInput; // the audio input and its channels, or why there's none
    std::function<float()> inputLevel;        // the input's peak this frame
    std::function<std::string()> notice;      // a recent message about recording, or empty
    std::function<int()> countIn;             // the count-in choice: 0 off, 1 beat, 2 a bar, 3 two bars
    std::function<void(int)> setCountIn;
};

// The bar across the top: return to start, play/pause, stop and record; the position in bars,
// beats and ticks over the clock time; tempo and time signature; the metronome; the inputs in use
// (MIDI with an activity light, the audio input with a meter); and the output meters. Everything
// here drives the audio system's real transport.
class TransportBar : public ui::Panel {
public:
    TransportBar(ui::Context &context, audio::AudioSystem &audio, audio::Metronome &metronome, ProjectStore &store,
                 const StereoLevels &levels, RecordHooks record);

    void handleInput(const ui::Input &input) override;

private:
    audio::AudioSystem &audio_;
    const StereoLevels &levels_;
    RecordHooks record_;
    ui::Button *play_ = nullptr;
    ui::Button *recordButton_ = nullptr;
    ui::LevelMeter *inputMeter_ = nullptr;
    ui::Stepper *tempo_ = nullptr;
    ui::LevelMeter *meters_[2] = {};
};

}
