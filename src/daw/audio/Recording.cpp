#include "daw/audio/Recording.h"

#include "daw/model/NoteTools.h"
#include "daw/model/Takes.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <ctime>
#include <filesystem>
#include <system_error>

namespace daw {

namespace fs = std::filesystem;

namespace {

// A file name from a track name: characters Windows refuses become spaces
std::string fileSafe(std::string name)
{
    for (char &c : name) {
        if (std::string("<>:\"/\\|?*").find(c) != std::string::npos || static_cast<unsigned char>(c) < 32)
            c = ' ';
    }
    return name.empty() ? std::string("Track") : name;
}

std::string timestamp()
{
    const std::time_t now = std::time(nullptr);
    char buffer[32];
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%d %H-%M-%S", std::localtime(&now));
    return buffer;
}

}

RecordingSession::RecordingSession(ProjectStore &store, audio::AudioSystem &audio, audio::MidiHub &hub,
                                   audio::InputRecorder &recorder, AudioPool &pool)
    : store_(store), audio_(audio), hub_(hub), recorder_(recorder), pool_(pool)
{
}

bool RecordingSession::start(const std::string &takeFolder, std::string &why, double audioDelaySeconds)
{
    if (recording_ || finishing_) {
        why = "Still finishing the last take";
        return false;
    }
    targets_.clear();
    audioTargets_ = false;
    bool audioWithoutInput = false;
    for (size_t i = 0; i < project().tracks.size(); ++i) {
        const Track &track = project().tracks[i];
        if (!track.armed || track.bus()) // buses record nothing
            continue;
        Target t;
        t.track = static_cast<int>(i);
        t.audio = track.kind == TrackKind::Audio;
        if (t.audio && audio_.inputChannels() == 0) {
            audioWithoutInput = true;
            continue;
        }
        t.heldStart.fill(-1.0);
        audioTargets_ = audioTargets_ || t.audio;
        targets_.push_back(t);
    }
    if (targets_.empty()) {
        why = audioWithoutInput ? "No audio input is open: choose one in Settings, under Audio"
                                : "Arm a track to record on (its R button)";
        return false;
    }
    why = audioWithoutInput ? "Some armed audio tracks have no input open, so they won't record" : "";

    folder_ = takeFolder;
    ++takeNumber_;
    take_.clear();
    takeChannels_ = 0;
    pedal_ = false;
    audio::Transport &transport = audio_.transport();
    const double rate = audio_.sampleRate() > 0 ? audio_.sampleRate() : 48000.0;
    const double beat = project().timing.beatAtSample(static_cast<double>(transport.position()), rate);
    // Loop recording, when the transport loops and the take starts before the loop's end
    looping_ = transport.looping() && transport.loopEnd() > transport.loopStart() && beat < transport.loopEnd();
    loopStartBeat_ = transport.loopStart();
    loopEndBeat_ = transport.loopEnd();
    loopStartSample_ = transport.sampleOf(loopStartBeat_, rate);
    loopEndSample_ = transport.sampleOf(loopEndBeat_, rate);
    startLap_ = transport.laps();
    for (Target &t : targets_)
        t.lap = startLap_;
    // MIDI clips exist from the start, from the bar the take starts in
    for (Target &t : targets_) {
        if (t.audio)
            continue;
        Track &track = store_.editTrack(t.track);
        const int bar = project().timing.barAt(beat);
        t.clipStart = project().timing.barStart(bar);
        t.clipName = "Take " + std::to_string(takeNumber_);
        track.clips.push_back({t.clipName, t.clipStart, project().timing.barLength(bar)});
        t.clip = static_cast<int>(track.clips.size()) - 1;
    }
    if (audioTargets_) {
        if (audioDelaySeconds > 0.0) {
            armPending_ = true;
            armAt_ = static_cast<double>(transport.position()) + audioDelaySeconds * rate;
        } else {
            recorder_.arm();
        }
    }
    if (!transport.playing())
        transport.play();
    recording_ = true;
    return true;
}

int RecordingSession::clipIndex(Target &t) const
{
    const Track *track = project().track(t.track);
    if (!track || t.clip < 0)
        return -1;
    auto matches = [&t](const Clip &c) { return c.name == t.clipName && std::fabs(c.start - t.clipStart) < 1e-6; };
    if (t.clip < static_cast<int>(track->clips.size()) && matches(track->clips[static_cast<size_t>(t.clip)]))
        return t.clip;
    // Something moved it (another clip was deleted): find it again
    for (size_t i = 0; i < track->clips.size(); ++i) {
        if (matches(track->clips[i])) {
            t.clip = static_cast<int>(i);
            return t.clip;
        }
    }
    return -1;
}

const Clip *RecordingSession::clipOf(Target &t) const
{
    const int index = clipIndex(t);
    const Track *track = project().track(t.track);
    return index < 0 || !track ? nullptr : &track->clips[static_cast<size_t>(index)];
}

Clip *RecordingSession::editClipOf(Target &t)
{
    const int index = clipIndex(t);
    return index < 0 ? nullptr : &store_.editClip(t.track, index);
}

double RecordingSession::beatOf(const audio::StampedMidi &m) const
{
    // The player heard the output a buffer late, so what they played belongs that much earlier
    const double latency = audio_.bufferFrames();
    const double position = std::max(0.0, static_cast<double>(m.position) - latency);
    return project().timing.beatAtSample(position, m.sampleRate);
}

void RecordingSession::endNote(Target &t, int key, double beat)
{
    const size_t k = static_cast<size_t>(key);
    if (t.heldStart[k] < 0.0)
        return;
    if (Clip *clip = editClipOf(t)) {
        Note n;
        n.start = std::max(0.0, t.heldStart[k] - t.clipStart);
        n.length = std::max(1.0 / 64.0, beat - t.heldStart[k]);
        n.key = key;
        n.velocity = t.heldVelocity[k];
        clip->notes.push_back(n);
    }
    t.heldStart[k] = -1.0;
    t.sustained[k] = false;
}

void RecordingSession::midi(const audio::StampedMidi &m)
{
    if (!recording_ || !m.playing)
        return;
    const double beat = beatOf(m);
    const audio::MidiMessage &msg = m.message;
    for (Target &t : targets_) {
        if (t.audio)
            continue;
        if (looping_ && m.lap > t.lap)
            lapMidi(t, m.lap);
        const size_t key = msg.data1;
        if (msg.isNoteOn()) {
            if (t.heldStart[key] >= 0.0)
                endNote(t, msg.data1, beat); // struck again before its release
            t.heldStart[key] = beat;
            t.heldVelocity[key] = msg.data2 / 127.0f;
        } else if (msg.isNoteOff()) {
            if (pedal_)
                t.sustained[key] = t.heldStart[key] >= 0.0;
            else
                endNote(t, msg.data1, beat);
        }
        // Controllers (not the pedal, which the notes' lengths already hold, nor the channel
        // modes) and the pitch bend go in the take's lanes
        const bool controller = msg.type() == 0xB0 && msg.data1 != 64 && msg.data1 < 120;
        if (controller || msg.type() == 0xE0) {
            if (Clip *clip = editClipOf(t)) {
                const int number = controller ? msg.data1 : kPitchBend;
                const float value = controller ? msg.data2 / 127.0f : static_cast<float>(msg.data1 | (msg.data2 << 7)) / 16383.0f;
                setControlPoint(laneFor(clip->controls, number), std::max(0.0, beat - t.clipStart), value, 1.0 / 256.0);
            }
        }
    }
    if (msg.isController(64)) {
        pedal_ = msg.data2 >= 64;
        if (!pedal_) {
            for (Target &t : targets_) {
                for (int k = 0; k < 128; ++k) {
                    if (t.sustained[static_cast<size_t>(k)])
                        endNote(t, k, beat);
                }
            }
        }
    }
}

void RecordingSession::growAudio(size_t newFrames)
{
    const double rate = audio_.sampleRate() > 0 ? audio_.sampleRate() : 48000.0;
    const size_t total = take_.size() / std::max<uint32_t>(1, takeChannels_);
    for (Target &t : targets_) {
        if (!t.audio || !t.preview)
            continue;
        const Track &track = project().tracks[static_cast<size_t>(t.track)];
        const uint32_t first = static_cast<uint32_t>(std::clamp(track.inputChannel, 0, static_cast<int>(takeChannels_) - 1));
        const uint32_t second = track.inputStereo && first + 1 < takeChannels_ ? first + 1 : first;
        for (size_t f = total - newFrames; f < total; ++f) {
            const float l = take_[f * takeChannels_ + first];
            const float r = take_[f * takeChannels_ + second];
            t.runLow = std::min({t.runLow, l, r});
            t.runHigh = std::max({t.runHigh, l, r});
            if (++t.runFrames == audio::AudioData::kPeakFrames) {
                t.preview->peaks.push_back(t.runLow);
                t.preview->peaks.push_back(t.runHigh);
                t.runLow = t.runHigh = 0.0f;
                t.runFrames = 0;
            }
        }
        t.preview->frames = total;
        // Loop recording: a pass ends where the loop wrapped. The take's frame f was heard at
        // transport sample start + f - latency, so the first wrap is (loop end - start + latency)
        // frames in, and each lap a loop's length later.
        if (looping_ && recorder_.started() && recorder_.startPosition() < loopEndSample_ && loopEndSample_ > loopStartSample_) {
            const size_t latency = static_cast<size_t>(audio_.roundTripFrames());
            const size_t lap = static_cast<size_t>(loopEndSample_ - loopStartSample_);
            for (;;) {
                const size_t boundary = t.passNames.empty()
                                            ? static_cast<size_t>(loopEndSample_ - recorder_.startPosition()) + latency
                                            : t.passFrame + lap;
                if (total < boundary)
                    break;
                if (const Clip *clip = clipOf(t)) {
                    const double length = std::max(1.0 / 64.0, project().beatsFor(clip->start, boundary / rate - clip->offset));
                    editClipOf(t)->length = length;
                }
                endPass(t);
                startPass(t, loopStartBeat_, boundary / rate);
                t.passFrame = boundary;
            }
        }
        if (const Clip *clip = clipOf(t)) {
            // Only when the length really moves (this runs every frame of an audio take)
            const double length = std::max(1.0 / 64.0, project().beatsFor(clip->start, total / rate - clip->offset));
            if (length != clip->length)
                editClipOf(t)->length = length;
        }
    }
}

std::string RecordingSession::update()
{
    std::string message;
    audio::StampedMidi m;
    while (hub_.takeStamped(m)) {
        ++midiMessages_;
        midi(m);
    }

    if (recording_ && !audio_.transport().playing())
        stop();
    // Between takes the recorder's ring goes (once the audio thread is out of it)
    if (!recording_ && !finishing_ && !armPending_)
        recorder_.releaseMemory();

    // A delayed audio start: arm the recorder once the transport reaches its beat
    if (armPending_ && recording_ && static_cast<double>(audio_.transport().position()) >= armAt_) {
        armPending_ = false;
        recorder_.arm();
    }

    if (recording_) {
        // MIDI clips reach the bar the playhead is in
        const double rate = audio_.sampleRate() > 0 ? audio_.sampleRate() : 48000.0;
        const audio::TempoMap &timing = project().timing;
        const double beat = timing.beatAtSample(static_cast<double>(audio_.transport().position()), rate);
        double barEnd = timing.barStart(timing.barAt(beat) + 1);
        if (looping_)
            barEnd = std::min(barEnd, loopEndBeat_); // a lap's pass ends at the loop's end
        const uint32_t lap = audio_.transport().laps();
        for (Target &t : targets_) {
            if (t.audio)
                continue;
            if (looping_ && lap > t.lap)
                lapMidi(t, lap);
            if (const Clip *clip = clipOf(t)) {
                const double length = std::max(clip->length, barEnd - t.clipStart);
                if (length != clip->length)
                    editClipOf(t)->length = length;
            }
        }
    }

    if (audioTargets_ && (recording_ || finishing_) && recorder_.started()) {
        if (takeChannels_ == 0) {
            // The take has begun: its clips, where the input really was (a round trip earlier)
            takeChannels_ = recorder_.channels();
            const double rate = audio_.sampleRate() > 0 ? audio_.sampleRate() : 48000.0;
            const double latency = audio_.roundTripFrames();
            const double startFrames = static_cast<double>(recorder_.startPosition()) - latency;
            for (Target &t : targets_) {
                if (!t.audio)
                    continue;
                Track &track = store_.editTrack(t.track);
                Clip clip;
                clip.name = t.clipName = "Take " + std::to_string(takeNumber_);
                clip.start = t.clipStart = project().timing.beatAtSample(std::max(0.0, startFrames), rate);
                clip.offset = std::max(0.0, -startFrames) / rate; // what came before the timeline's start
                clip.length = 1.0 / 64.0;
                track.clips.push_back(clip);
                t.clip = static_cast<int>(track.clips.size()) - 1;
                t.preview = std::make_shared<audio::AudioData>();
                t.preview->sampleRate = rate;
            }
        }
        const size_t added = recorder_.drain(take_);
        if (added > 0)
            growAudio(added);
    }

    if (finishing_ && !recorder_.capturing())
        message = finish();
    return message;
}

void RecordingSession::stop()
{
    if (!recording_)
        return;
    recording_ = false;
    const double rate = audio_.sampleRate() > 0 ? audio_.sampleRate() : 48000.0;
    const audio::TempoMap &timing = project().timing;
    const double beat = timing.beatAtSample(std::max(0.0, static_cast<double>(audio_.transport().position()) - audio_.bufferFrames()), rate);
    for (Target &t : targets_) {
        if (t.audio)
            continue;
        if (looping_)
            lapMidi(t, audio_.transport().laps());
        for (int k = 0; k < 128; ++k)
            endNote(t, k, beat);
        const Clip *clip = clipOf(t);
        if (!project().track(t.track))
            continue;
        if (clip && clip->notes.empty() && clip->controls.empty()) {
            // Nothing was played in the last pass: no clip
            Track &track = store_.editTrack(t.track);
            track.clips.erase(track.clips.begin() + t.clip);
            t.clip = -1;
            if (project().selectedTrack == t.track && project().selectedClip >= 0)
                store_.select(t.track, -1);
        } else if (clip) {
            Clip *edited = editClipOf(t);
            // The controllers' runs of messages, thinned to the points their curves need
            for (ControlLane &lane : edited->controls)
                thinControlPoints(lane, 1.0f / 254.0f);
            double end = beat;
            for (const Note &n : edited->notes)
                end = std::max(end, t.clipStart + n.start + n.length);
            // To the bar line at or after the end, at least a bar (within the loop, looping)
            const double barEnd = timing.barStart(timing.barAt(end - 1e-9) + 1);
            double clipEnd = std::max(timing.barStart(timing.barAt(t.clipStart) + 1), barEnd);
            if (looping_ && t.clipStart < loopEndBeat_)
                clipEnd = std::min(clipEnd, loopEndBeat_);
            edited->length = std::max(1.0 / 64.0, clipEnd - t.clipStart);
        }
        // The finished passes' controllers too
        Track &track = store_.editTrack(t.track);
        for (TakeLane &lane : track.takeLanes) {
            if (std::find(t.passNames.begin(), t.passNames.end(), lane.name) == t.passNames.end())
                continue;
            for (Clip &c : lane.clips) {
                for (ControlLane &curve : c.controls)
                    thinControlPoints(curve, 1.0f / 254.0f);
            }
        }
        fold(t);
    }
    if (audioTargets_) {
        armPending_ = false;
        recorder_.disarm();
        finishing_ = true;
    } else {
        targets_.clear();
    }
}

std::string RecordingSession::finish()
{
    finishing_ = false;
    std::string message;
    if (recorder_.started()) {
        const size_t added = recorder_.drain(take_);
        if (added > 0)
            growAudio(added);
    }
    const double rate = audio_.sampleRate() > 0 ? audio_.sampleRate() : 48000.0;
    const size_t frames = takeChannels_ ? take_.size() / takeChannels_ : 0;
    std::error_code ec;
    fs::create_directories(folder_, ec);
    for (Target &t : targets_) {
        if (!t.audio)
            continue;
        const Track *track = project().track(t.track);
        const Clip *clip = clipOf(t);
        if (!track || !clip)
            continue;
        if (frames == 0) {
            Track &edited = store_.editTrack(t.track);
            edited.clips.erase(edited.clips.begin() + t.clip);
            edited.takeLanes.erase(std::remove_if(edited.takeLanes.begin(), edited.takeLanes.end(), [&t](const TakeLane &lane) {
                return std::find(t.passNames.begin(), t.passNames.end(), lane.name) != t.passNames.end();
            }), edited.takeLanes.end());
            message = "Nothing came in from the input";
            continue;
        }
        // This track's channel, or its pair
        const uint32_t first = static_cast<uint32_t>(std::clamp(track->inputChannel, 0, static_cast<int>(takeChannels_) - 1));
        const bool stereo = track->inputStereo && first + 1 < takeChannels_;
        const uint32_t channels = stereo ? 2 : 1;
        std::vector<float> samples(frames * channels);
        for (size_t f = 0; f < frames; ++f) {
            samples[f * channels] = take_[f * takeChannels_ + first];
            if (stereo)
                samples[f * channels + 1] = take_[f * takeChannels_ + first + 1];
        }
        const std::string path =
            (fs::path(folder_) / (fileSafe(track->name) + " " + clip->name + " " + timestamp() + ".wav")).string();
        std::string error;
        if (!audio::writeWavFile(path, samples.data(), frames, channels, rate, error)) {
            message = error;
            Track &edited = store_.editTrack(t.track);
            edited.clips.erase(edited.clips.begin() + t.clip);
            continue;
        }
        pool_.addRecording(path, rate, channels, std::move(samples)); // streams from the file if long
        Clip &edited = *editClipOf(t);
        edited.audioFile = path;
        edited.length = std::max(1.0 / 64.0, project().beatsFor(edited.start, frames / rate - edited.offset));
        // Every pass is the one file, at its own offset
        for (TakeLane &lane : store_.editTrack(t.track).takeLanes) {
            if (std::find(t.passNames.begin(), t.passNames.end(), lane.name) == t.passNames.end())
                continue;
            for (Clip &c : lane.clips)
                c.audioFile = path;
        }
        fold(t);
    }
    if (recorder_.dropped() > 0)
        message = "The input fell behind: " + std::to_string(recorder_.dropped() * 1000 / static_cast<uint64_t>(rate)) +
                  " ms of it were lost";
    targets_.clear();
    take_.clear();
    take_.shrink_to_fit();
    recorder_.releaseMemory();
    return message;
}

void RecordingSession::endPass(Target &t)
{
    const int index = clipIndex(t);
    if (index < 0)
        return;
    Track &track = store_.editTrack(t.track);
    Clip pass = track.clips[static_cast<size_t>(index)];
    track.clips.erase(track.clips.begin() + index);
    if (project().selectedTrack == t.track && project().selectedClip >= 0)
        store_.select(t.track, -1);
    t.passNames.push_back(pass.name);
    TakeLane lane;
    lane.name = pass.name;
    lane.clips.push_back(std::move(pass));
    track.takeLanes.insert(track.takeLanes.begin(), std::move(lane));
    t.clip = -1;
}

void RecordingSession::startPass(Target &t, double start, double offset)
{
    Track &track = store_.editTrack(t.track);
    Clip clip;
    clip.name = t.clipName = "Take " + std::to_string(++takeNumber_);
    clip.start = t.clipStart = start;
    clip.offset = offset;
    const audio::TempoMap &timing = project().timing;
    clip.length = t.audio ? 1.0 / 64.0 : std::max(1.0 / 64.0, std::min(timing.barStart(timing.barAt(start) + 1), loopEndBeat_) - start);
    track.clips.push_back(clip);
    t.clip = static_cast<int>(track.clips.size()) - 1;
}

void RecordingSession::lapMidi(Target &t, uint32_t lap)
{
    while (t.lap < lap) {
        // The pass ends at the loop's end, notes held over it too
        for (int k = 0; k < 128; ++k)
            endNote(t, k, loopEndBeat_);
        if (const Clip *clip = clipOf(t)) {
            const double length = std::max(1.0 / 64.0, loopEndBeat_ - t.clipStart);
            if (clip->length != length)
                editClipOf(t)->length = length;
        }
        endPass(t);
        startPass(t, loopStartBeat_, 0.0);
        ++t.lap;
    }
}

void RecordingSession::fold(Target &t)
{
    if (!project().track(t.track))
        return;
    Track &track = store_.editTrack(t.track);
    // The passes in the order they were played: the finished ones from their lanes, then the last
    std::vector<Clip> passes;
    for (const std::string &name : t.passNames) {
        auto lane = std::find_if(track.takeLanes.begin(), track.takeLanes.end(),
                                 [&name](const TakeLane &l) { return l.name == name && l.clips.size() == 1; });
        if (lane == track.takeLanes.end())
            continue; // deleted while recording
        passes.push_back(lane->clips.front());
        track.takeLanes.erase(lane);
    }
    const int index = clipIndex(t);
    if (index >= 0) {
        passes.push_back(track.clips[static_cast<size_t>(index)]);
        track.clips.erase(track.clips.begin() + index);
    }
    t.passNames.clear();
    t.clip = -1;
    // A MIDI pass with nothing played in it isn't a take
    if (!t.audio)
        passes.erase(std::remove_if(passes.begin(), passes.end(), [](const Clip &c) { return c.notes.empty() && c.controls.empty(); }),
                     passes.end());
    if (passes.empty())
        return;
    if (project().selectedTrack == t.track)
        store_.select(t.track, -1);
    if (foldTakes(track, std::move(passes), project().timing))
        newTakes_.push_back(track.id);
}

std::vector<ObjectId> RecordingSession::takeNewTakes()
{
    std::vector<ObjectId> tracks = std::move(newTakes_);
    newTakes_.clear();
    return tracks;
}

const audio::AudioData *RecordingSession::trackPreview(int track) const
{
    for (const Target &t : targets_) {
        if (t.audio && t.track == track)
            return t.preview.get();
    }
    return nullptr;
}

const audio::AudioData *RecordingSession::preview(int track, int clip) const
{
    for (const Target &t : targets_) {
        if (t.audio && t.track == track && t.clip == clip)
            return t.preview.get();
    }
    return nullptr;
}

bool RecordingSession::isRecording(int track, int clip) const
{
    if (!recording_ && !finishing_)
        return false;
    for (const Target &t : targets_) {
        if (t.track == track && t.clip == clip)
            return true;
    }
    return false;
}

}
