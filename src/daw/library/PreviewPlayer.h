#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "audio/Device.h"
#include "audio/NotePlayer.h"
#include "audio/Preview.h"
#include "audio/SampleSet.h"

namespace audio {
class AudioSystem;
class SynthDevice;
}
namespace engine {
class Window;
}
namespace daw {

class AudioPool;
class InstrumentLibrary;

// The asset browser's listening previews, outside the project: a sound file played from its start, or
// an instrument's short phrase through a sampler of its own. Both are processors in the audio graph
// (made here, after the recorder and before the tracks, as the DawApp always made them), and nothing
// here touches the project. The built-in synthesizer's presets are heard through a synthesizer of
// its own, set to the preset's values. Called from the main thread once a frame (update), which
// starts each preview once its sound has loaded and plays the phrase's notes.
class PreviewPlayer {
public:
    PreviewPlayer(audio::AudioSystem &audio, InstrumentLibrary &library, AudioPool &pool, engine::Window &window,
                  std::function<void(const std::string &)> notify);

    PreviewPlayer(const PreviewPlayer &) = delete;
    PreviewPlayer &operator=(const PreviewPlayer &) = delete;

    // Listening: a phrase on an instrument, or a sound file; stop; whether one is playing
    void previewInstrument(const std::string &name);
    void previewSound(const std::string &path);
    // A synthesizer preset: its values are applied to the preview synthesizer and a phrase played
    void previewSynth(const audio::DeviceSetting &setting);
    void stopPreview();
    bool previewing() const;

    // Once a frame: starts previews once their sounds are loaded and plays the phrase's notes
    void update();

private:
    struct PhraseStep {
        double at;
        int key;
        float velocity;
        bool on;
    };
    // An arpeggio and a chord on a root key (the instrument previews use it too)
    static std::vector<PhraseStep> chordPhrase(int root);

    audio::SoundPreview &soundPreview_;
    audio::NotePlayer &previewPlayer_;
    audio::NotePlayer &synthPlayer_;
    audio::SynthDevice *synth_ = nullptr; // the preview synthesizer, owned by synthPlayer_
    InstrumentLibrary &library_;
    AudioPool &pool_;
    engine::Window &window_;
    std::function<void(const std::string &)> notify_;
    // The instrument asked for, its set once loaded, the sound waiting for the pool, and the phrase
    // being played (seconds from phraseStart_)
    std::string previewName_;
    bool previewLoading_ = false;
    std::shared_ptr<const audio::SampleSet> previewSet_;
    std::string previewPath_;
    std::vector<PhraseStep> phrase_;
    size_t phraseNext_ = 0;
    double phraseStart_ = 0.0;
    bool previewHeld_ = false; // the preview sampler still holds the last instrument previewed
    bool phraseOnSynth_ = false; // the phrase being played is the synthesizer's
};

}
