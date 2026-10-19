#pragma once

#include <functional>
#include <string>

#include "Project.h"
#include "ProjectStore.h"
#include "Recording.h"
#include "Settings.h"
#include "audio/AudioSystem.h"
#include "audio/Processors.h"

namespace daw {

// Starting and stopping takes onto the armed tracks, and the count-in before one. The RecordingSession
// makes the takes; this decides when to start them: an armed audio track opens the audio input
// first, and with a count-in set the metronome clicks the beats (the transport stays where the
// playhead is) and the take starts when the count ends, from the playhead.
class RecordingController {
public:
    struct Hooks {
        std::function<void()> openAudioInput; // the audio input, when an armed audio track needs it
        std::function<void(std::string)> notify;
    };

    RecordingController(audio::AudioSystem &audio, audio::Metronome &metronome, ProjectStore &store,
                        RecordingSession &recording, const Settings &settings, const std::string &projectPath,
                        Hooks hooks);

    RecordingController(const RecordingController &) = delete;
    RecordingController &operator=(const RecordingController &) = delete;

    // Starts recording onto the armed tracks (after the count-in, if one is set), or stops
    void toggleRecording();
    // Where takes are written: beside the saved project, else in the library's Recordings
    std::string takeFolder() const;

    // Count-in: what the transport bar shows (0 off, 1 beat, 2 one bar, 3 two bars); while counting
    // in the transport pre-rolls and then the take starts
    int countIn() const { return countInChoice_; }
    void setCountIn(int choice) { countInChoice_ = choice; }
    bool countingIn() const { return countingIn_; }
    void beginCountIn();
    void cancelCountIn();
    void updateCountIn(); // once a frame

private:
    audio::AudioSystem &audio_;
    audio::Metronome &metronome_;
    ProjectStore &store_;
    RecordingSession &recording_;
    const Settings &settings_;
    const std::string &projectPath_;
    Hooks hooks_;
    int countInChoice_ = 0;
    bool countingIn_ = false;
};

}
