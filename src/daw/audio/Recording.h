#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "daw/audio/AudioPool.h"
#include "daw/model/ProjectStore.h"
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

    // For drawing: the overview of a clip being recorded, or null; and of the audio being recorded on
    // a track (every pass of it, which take lanes show at their offsets), or null
    const audio::AudioData *preview(int track, int clip) const;
    const audio::AudioData *trackPreview(int track) const;
    // The tracks (by id) whose recordings left takes since the last call, so their lanes can show
    std::vector<ObjectId> takeNewTakes();
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
        // Loop recording: each lap is a pass. The current pass is the clip above; the finished ones
        // wait in lanes of their own, named as their clips (passNames, in order), until the recording
        // ends and every pass is folded into takes (foldTakes).
        std::vector<std::string> passNames;
        uint32_t lap = 0;     // the transport's lap the current pass is in
        size_t passFrame = 0; // audio: the take's frame where the current pass starts
    };

    const Project &project() const { return store_.project(); }
    // The index of a target's clip in its track (found again when something moved it), or -1
    int clipIndex(Target &t) const;
    const Clip *clipOf(Target &t) const;
    // The same clip, for changing (nothing is counted as changed when there is none)
    Clip *editClipOf(Target &t);
    double beatOf(const audio::StampedMidi &m) const;
    // A pass ends: its clip moves into a lane of its own; the next starts at `start` (audio: `offset`
    // seconds into the take)
    void endPass(Target &t);
    void startPass(Target &t, double start, double offset);
    // MIDI: passes end at the loop's end until the target is in `lap`
    void lapMidi(Target &t, uint32_t lap);
    // The recording on this target is over: its passes become takes (or a plain clip)
    void fold(Target &t);
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
    // Loop recording: whether the transport loops over the take, the loop in beats and samples, and
    // the lap it started in
    bool looping_ = false;
    double loopStartBeat_ = 0.0, loopEndBeat_ = 0.0;
    uint64_t loopStartSample_ = 0, loopEndSample_ = 0;
    uint32_t startLap_ = 0;
    std::vector<ObjectId> newTakes_;
};

}
