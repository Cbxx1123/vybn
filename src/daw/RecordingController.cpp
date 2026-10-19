// Starting and stopping takes, and the count-in before one (the recording controls of DawApp)
#include "RecordingController.h"

#include <algorithm>
#include <filesystem>
#include <utility>

namespace daw {

namespace fs = std::filesystem;

RecordingController::RecordingController(audio::AudioSystem &audio, audio::Metronome &metronome, ProjectStore &store,
                                         RecordingSession &recording, const Settings &settings,
                                         const std::string &projectPath, Hooks hooks)
    : audio_(audio), metronome_(metronome), store_(store), recording_(recording), settings_(settings),
      projectPath_(projectPath), hooks_(std::move(hooks))
{
}

std::string RecordingController::takeFolder() const
{
    if (projectPath_.empty())
        return (fs::path(settings_.libraryPath) / "Recordings").string();
    const fs::path project(projectPath_);
    return (project.parent_path() / (project.stem().string() + " Audio")).string();
}

void RecordingController::toggleRecording()
{
    if (countingIn_) {
        cancelCountIn(); // pressed again during the count-in
        return;
    }
    if (recording_.recording()) {
        recording_.stop(); // the transport keeps playing
        return;
    }
    if (recording_.finishing())
        return;
    // An armed audio track needs the input
    bool anyArmed = false;
    bool armedAudio = false;
    for (const Track &t : store_.project().tracks) {
        anyArmed = anyArmed || t.armed;
        armedAudio = armedAudio || (t.armed && t.kind == TrackKind::Audio);
    }
    if (armedAudio && audio_.inputChannels() == 0 && settings_.audioInput != Settings::kNoInput)
        hooks_.openAudioInput();
    if (countInChoice_ > 0 && anyArmed) {
        beginCountIn();
        return;
    }
    std::string why;
    if (!recording_.start(takeFolder(), why) || !why.empty())
        hooks_.notify(why);
}

void RecordingController::beginCountIn()
{
    // Clicks only: the transport stays where the playhead is, and starts the track when the count
    // ends, so the track and the take begin together from that position
    const int bar = std::max(1, store_.project().beatsPerBar);
    const int beats = countInChoice_ == 1 ? 1 : countInChoice_ == 2 ? bar : 2 * bar;
    audio_.transport().stop();
    metronome_.startCountIn(beats);
    countingIn_ = true;
}

void RecordingController::cancelCountIn()
{
    metronome_.cancelCountIn();
    countingIn_ = false;
    audio_.transport().stop();
}

void RecordingController::updateCountIn()
{
    if (!countingIn_ || !metronome_.countInDone())
        return;
    countingIn_ = false;
    // The audio input is held back by one round trip, so the take's audio begins on the playhead
    // rather than a little before it
    const double rate = audio_.sampleRate() > 0 ? audio_.sampleRate() : 48000.0;
    const double delay = static_cast<double>(audio_.roundTripFrames()) / rate;
    std::string why;
    if (!recording_.start(takeFolder(), why, delay) || !why.empty())
        hooks_.notify(why);
}

}
