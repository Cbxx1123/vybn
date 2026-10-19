#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "AudioPool.h"
#include "ProjectStore.h"
#include "audio/AudioSystem.h"
#include "audio/Midi.h"
#include "audio/Recorder.h"

namespace daw {

// Recording onto the armed tracks. start() plays the transport and begins a take on every armed
// track: instrument tracks get a new clip (from the bar the take starts in) that MIDI notes are
// written into as they are played, with the sustain pedal lengthening them; audio tracks get a
// clip that grows with the input (its channel or pair, from the track's input setting), drawn
// from a live overview. stop() (or the transport stopping) ends the take: MIDI clips are cut to
// whole bars (empty ones are dropped), and each audio take is written as a WAV file into the take
// folder and becomes the clip's sound file. Recorded audio and MIDI are moved earlier by the
// device's latency, so they line up with what was playing.
//
// update() runs once a frame on the main thread. It always reads the MIDI hub's stamped messages
// (so they never pile up), whether recording or not.
class RecordingSession {
public:
    RecordingSession(ProjectStore &store, audio::AudioSystem &audio, audio::MidiHub &hub, audio::InputRecorder &recorder,
                     AudioPool &pool);

    // Returns false, with why, when nothing is armed (or an armed audio track has no input)
    // audioDelaySeconds holds the audio input back that long (the take's audio starts that much
    // later on the transport), so it lines up with a count-in's last beat
    bool start(const std::string &takeFolder, std::string &why, double audioDelaySeconds = 0.0);
    void stop();
    bool recording() const { return recording_; }
    // Still writing takes after stop()
    bool finishing() const { return finishing_; }
    // Once a frame. Returns a message worth showing (a failure, lost audio), or empty.
    std::string update();

    // For drawing: the overview of a clip being recorded, or null
    const audio::AudioData *preview(int track, int clip) const;
    bool isRecording(int track, int clip) const;
    // Every MIDI message seen, for an activity light
    uint64_t midiMessages() const { return midiMessages_; }

private:
    struct Target {
        int track = -1;
        int clip = -1;         // index into the track's clips, -1 until it exists
        std::string clipName;
        double clipStart = 0.0;
        bool audio = false;
        // MIDI: keys held (start beat, velocity; start < 0 when up), and released under the pedal
        std::array<double, 128> heldStart{};
        std::array<float, 128> heldVelocity{};
        std::array<bool, 128> sustained{};
        // Audio: the live overview (peaks only, grown as the take does)
        std::shared_ptr<audio::AudioData> preview;
        float runLow = 0.0f, runHigh = 0.0f;
        uint32_t runFrames = 0;
    };

    const Project &project() const { return store_.project(); }
    // The index of a target's clip in its track (found again when something moved it), or -1
    int clipIndex(Target &t) const;
    const Clip *clipOf(Target &t) const;
    // The same clip, for changing (nothing is counted as changed when there is none)
    Clip *editClipOf(Target &t);
    double beatOf(const audio::StampedMidi &m) const;
    void midi(const audio::StampedMidi &m);
    void endNote(Target &t, int key, double beat);
    void growAudio(size_t newFrames);
    std::string finish();

    ProjectStore &store_;
    audio::AudioSystem &audio_;
    audio::MidiHub &hub_;
    audio::InputRecorder &recorder_;
    AudioPool &pool_;
    std::vector<Target> targets_;
    std::string folder_;
    bool recording_ = false;
    bool finishing_ = false;
    bool pedal_ = false;
    bool audioTargets_ = false;
    bool armPending_ = false; // the recorder is waiting for the transport to reach armAt_
    double armAt_ = 0.0;      // in transport samples
    std::vector<float> take_; // every input channel, interleaved
    uint32_t takeChannels_ = 0;
    uint64_t midiMessages_ = 0;
    int takeNumber_ = 0;
};

}
