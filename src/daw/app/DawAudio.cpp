// DawApp's inputs, recording, audio import and editing commands, and the project dialogs
#include "daw/app/DawApp.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "daw/views/Arrangement.h"
#include "daw/views/AudioEditor.h"
#include "daw/model/AutomationLanes.h"
#include "daw/widgets/Format.h"
#include "daw/views/Mixer.h"
#include "daw/views/PianoRoll.h"
#include "daw/model/Routing.h"
#include "daw/model/Takes.h"
#include "daw/views/TransportBar.h"
#include "audio/AudioFile.h"
#include "audio/Memory.h"
#include "engine/FileDialog.h"
#include "engine/Shell.h"
#include "env/Resources.h"
#include "env/Scheduler.h"

namespace daw {

using namespace ui;
namespace fs = std::filesystem;

namespace {

Container &line(Container &parent)
{
    auto &r = parent.add<Container>(Direction::Row);
    r.setAlign(Align::Center);
    return r;
}

Container &labelled(Container &parent, const char *label)
{
    auto &r = line(parent);
    r.add<Label>(label, TextStyle::Body, TextTone::Secondary).setMinSize({110.0f, 0.0f});
    return r;
}

// Calls `each` on every frame hook's frame: a zero-size component that feeds meters and lights
class Ticker : public Component {
public:
    explicit Ticker(std::function<void()> each) : each_(std::move(each)) {}
    Vec2 measure(const Theme &) const override { return {}; }
    void handleInput(const Input &) override { each_(); }

private:
    std::function<void()> each_;
};

}

// Inputs

void DawApp::applyAudioSettings()
{
    audio::AudioSettings s = audio_.settings();
    s.device = outputIndex();
    s.bufferFrames = settings_.bufferFrames;
    s.input = audio::AudioSettings::kNoInput; // opened once an audio track is armed
    audio_.configure(s);
}

int DawApp::outputIndex() const
{
    const auto &devices = audio_.devices();
    for (size_t i = 0; i < devices.size(); ++i) {
        if (!settings_.audioOutput.empty() && devices[i].name == settings_.audioOutput)
            return static_cast<int>(i);
    }
    return -1;
}

int DawApp::inputIndex() const
{
    const auto &devices = audio_.inputDevices();
    for (size_t i = 0; i < devices.size(); ++i) {
        if (!settings_.audioInput.empty() && devices[i].name == settings_.audioInput)
            return static_cast<int>(i);
    }
    return -1; // the system default (or the one chosen is unplugged)
}

void DawApp::openAudioInput()
{
    inputTried_ = true;
    audio::AudioSettings s = audio_.settings();
    s.input = settings_.audioInput == Settings::kNoInput ? audio::AudioSettings::kNoInput : inputIndex();
    audio_.configure(s);
    if (s.input != audio::AudioSettings::kNoInput && audio_.running() && audio_.inputChannels() == 0)
        notify("Couldn't open the audio input" + (audio_.lastError().empty() ? "" : ": " + audio_.lastError()));
}

void DawApp::syncInputs(bool force)
{
    // MIDI: what's plugged in, every two seconds. Each one opens unless turned off in Settings.
    if (force || env_.frame() % 120 == 0) {
        if (midi_.refresh())
            midiFailed_.clear();
        const auto &names = midi_.devices();
        for (size_t i = 0; i < names.size(); ++i) {
            const bool off = std::find(settings_.midiDisabled.begin(), settings_.midiDisabled.end(), names[i]) !=
                             settings_.midiDisabled.end();
            if (off) {
                midi_.close(i);
                continue;
            }
            if (midi_.isOpen(i) || midiFailed_.count(names[i]))
                continue;
            std::string error;
            if (!midi_.open(i, error)) {
                midiFailed_.insert(names[i]);
                notify(error);
            }
        }
    }
    // The audio input, the first time an audio track is armed (so the microphone isn't opened
    // before it's wanted)
    bool armedAudio = false;
    for (const Track &t : project().tracks)
        armedAudio = armedAudio || (t.armed && t.kind == TrackKind::Audio);
    if (armedAudio && !inputTried_ && settings_.audioInput != Settings::kNoInput && audio_.inputChannels() == 0)
        openAudioInput();
}

std::string DawApp::midiInputText() const
{
    return midi_.openNames();
}

std::string DawApp::audioInputText() const
{
    if (audio_.inputChannels() > 0)
        return audio_.inputName() + format(" (%.0f in)", static_cast<double>(audio_.inputChannels()));
    if (settings_.audioInput == Settings::kNoInput)
        return "off (choose one in Settings)";
    if (inputTried_)
        return "couldn't open the input";
    return "opens when you arm an audio track";
}

std::string DawApp::trackInputText(int index) const
{
    const Track *t = project().track(index);
    if (!t)
        return {};
    if (t->kind == TrackKind::Instrument) { // "MIDI in" has its own line above it in the header
        return t->hasInstrument() ? t->instrument.name : std::string("No instrument  (click to choose)");
    }
    if (t->bus()) // a bus: what it is and where it goes
        return std::string(kindName(t->kind)) + "  /  to " + outputName(project(), index) + extraDestinations(project(), index);
    if (t->inputChannel < 0)
        return "Audio  /  no input";
    std::string in = "In " + std::to_string(t->inputChannel + 1);
    if (t->inputStereo)
        in += "-" + std::to_string(t->inputChannel + 2);
    return "Audio  /  " + in + (t->monitor ? ", heard" : "");
}

void DawApp::notify(std::string message)
{
    notice_ = std::move(message);
    noticeUntil_ = ui_.window().time() + 6.0;
}

std::string DawApp::notice() const
{
    if (ui_.window().time() < noticeUntil_)
        return notice_;
    if (recordingController_->countingIn())
        return "Counting in";
    if (recording_->recording())
        return "Recording";
    if (recording_->finishing())
        return "Saving the take...";
    return {};
}

// Audio clips

const audio::AudioData *DawApp::clipAudio(int track, int clip)
{
    if (const audio::AudioData *take = recording_->preview(track, clip))
        return take;
    const Track *t = project().track(track);
    if (!t || clip < 0 || clip >= static_cast<int>(t->clips.size()))
        return nullptr;
    const Clip &c = t->clips[static_cast<size_t>(clip)];
    return c.hasAudio() ? pool_->get(c.audioFile).get() : nullptr;
}

// Importing audio

void DawApp::importAudio(int track)
{
    ui::FileDialogOptions options;
    options.title = "Import audio";
    options.filters = {{"Audio files (WAV, FLAC, MP3)", audio::kAudioFilePatterns}, {"All files", "*.*"}};
    options.multiple = true;
    options.action = "Import";
    options.remember = "audio";
    options.folder = engine::knownFolder(engine::KnownFolder::Music);
    chooseFiles(std::move(options), [this, track](std::vector<std::string> files) { importAudioFiles(files, track); });
}

void DawApp::importAudioFiles(const std::vector<std::string> &files, int track)
{
    // Onto the given track, else the selected audio track, else a new one
    const Track *target = project().track(track);
    int index = track;
    if (!target || target->kind != TrackKind::Audio) {
        index = project().selectedTrack;
        target = project().track(index);
    }
    if (!target || target->kind != TrackKind::Audio) {
        Project &added = session_.store().edit();
        index = added.addTrack(TrackKind::Audio);
        added.tracks[static_cast<size_t>(index)].name = fs::path(files.front()).stem().string();
    }
    double beat = std::round(transportBeats(audio_));
    std::string errors;
    int added = -1;
    for (const std::string &file : files) {
        std::string error;
        auto data = pool_->loadNow(file, error); // streamed if long
        if (!data) {
            errors += (errors.empty() ? "" : "\n") + error;
            continue;
        }
        Clip c(fs::path(file).stem().string(), beat, project().beatsFor(beat, data->seconds()));
        c.audioFile = file;
        auto &clips = session_.store().editTrack(index).clips;
        clips.push_back(c);
        added = static_cast<int>(clips.size()) - 1;
        beat += c.length;
    }
    // The timeline grows to hold them
    if (beat > project().lengthBeats())
        session_.store().edit().bars = project().timing.barAt(beat - 1e-9) + 2;
    if (added >= 0)
        session_.store().select(index, added);
    if (!errors.empty())
        Dialog::open(gui_, "Some files couldn't be imported", errors).setWidth(520.0f).addButton("OK", {}, true);
}

void DawApp::importAudioDemo()
{
    // A few seconds of plucked chords, written as a WAV file and imported like any other
    const double rate = 48000.0;
    const size_t frames = static_cast<size_t>(rate * 6.0);
    std::vector<float> samples(frames * 2, 0.0f);
    const double notes[] = {220.0, 261.63, 329.63, 196.0, 246.94, 293.66};
    for (int pluck = 0; pluck < 12; ++pluck) {
        const size_t start = static_cast<size_t>(pluck * 0.5 * rate);
        const double f = notes[pluck % 6];
        for (size_t i = 0; start + i < frames && i < static_cast<size_t>(rate * 1.2); ++i) {
            const double t = i / rate;
            const float v = static_cast<float>(0.35 * std::exp(-t * 4.0) * std::sin(2.0 * 3.14159265358979 * f * t));
            samples[(start + i) * 2] += v;
            samples[(start + i) * 2 + 1] += v * 0.8f;
        }
    }
    const std::string path = (fs::temp_directory_path() / "VYBN demo take.wav").string();
    std::string error;
    if (!audio::writeWavFile(path, samples.data(), frames, 2, rate, error)) {
        notify(error);
        return;
    }
    int track = -1;
    for (size_t i = 0; i < project().tracks.size() && track < 0; ++i) {
        if (project().tracks[i].kind == TrackKind::Audio)
            track = static_cast<int>(i);
    }
    ui_.post([this, path, track] {
        importAudioFiles({path}, track);
        if (project().clip()) {
            Clip &c = session_.store().editClip(project().selectedTrack, project().selectedClip);
            c.fadeIn = 0.05;
            c.fadeOut = 0.8;
        }
        showEditor();
    });
}

void DawApp::takesDemo()
{
    // Three passes over two bars, as loop recording leaves them: an audio track's in one file (plucks
    // that differ pass to pass), and the Lead track's melodies; each comped from all three
    const double rate = 48000.0;
    const audio::TempoMap &timing = project().timing;
    const double passSeconds = timing.secondsAt(8.0);
    const size_t passFrames = static_cast<size_t>(passSeconds * rate);
    std::vector<float> samples(passFrames * 3, 0.0f);
    const double notes[3][4] = {{220.0, 261.63, 329.63, 293.66}, {246.94, 293.66, 349.23, 329.63}, {196.0, 261.63, 392.0, 349.23}};
    for (int pass = 0; pass < 3; ++pass) {
        for (int pluck = 0; pluck < 8; ++pluck) {
            const size_t start = pass * passFrames + static_cast<size_t>(timing.secondsAt(pluck) * rate);
            const double f = notes[pass][pluck % 4];
            for (size_t i = 0; start + i < samples.size() && i < static_cast<size_t>(rate * 0.45); ++i) {
                const double t = i / rate;
                samples[start + i] += static_cast<float>((0.25 + 0.1 * pass) * std::exp(-t * 6.0) * std::sin(2.0 * 3.14159265358979 * f * t));
            }
        }
    }
    const std::string path = (fs::temp_directory_path() / "VYBN demo takes.wav").string();
    std::string error;
    if (!audio::writeWavFile(path, samples.data(), samples.size(), 1, rate, error)) {
        notify(error);
        return;
    }
    pool_->addRecording(path, rate, 1, std::move(samples));
    ObjectId id = 900000; // above the demo's ids; normalizeIds keeps them
    for (size_t i = 0; i < project().tracks.size(); ++i) {
        const Track &t = project().tracks[i];
        if (t.kind != TrackKind::Audio && t.name != "Lead")
            continue;
        const bool audio = t.kind == TrackKind::Audio;
        editTakes(static_cast<int>(i), [&](Track &tr) {
            tr.clips.erase(std::remove_if(tr.clips.begin(), tr.clips.end(), [](const Clip &c) { return c.start < 8.0; }), tr.clips.end());
            for (int pass = 0; pass < 3; ++pass) {
                Clip take("Take " + std::to_string(pass + 1), 0.0, 8.0);
                take.id = ++id;
                if (audio) {
                    take.audioFile = path;
                    take.offset = pass * passSeconds;
                } else {
                    const int melody[3][8] = {{69, 72, 76, 74, 72, 69, 67, 69}, {69, 71, 72, 76, 79, 76, 74, 72}, {64, 67, 69, 72, 71, 69, 67, 64}};
                    for (int n = 0; n < 8; ++n)
                        take.notes.push_back({static_cast<double>(n), 0.9, melody[pass][n], 0.8f});
                }
                tr.takeLanes.insert(tr.takeLanes.begin(), TakeLane{++id, take.name, {take}});
            }
            // The comp: the third pass's opening, the first's middle, the second's end
            compRange(tr, 0.0, 2.0, tr.takeLanes[0].clips[0].id, timing);
            compRange(tr, 2.0, 5.0, tr.takeLanes[2].clips[0].id, timing);
            compRange(tr, 5.0, 8.0, tr.takeLanes[1].clips[0].id, timing);
            return true;
        });
        arrangement_->showTakes(static_cast<int>(i), true);
        if (audio)
            break;
    }
}

void DawApp::recordMidiDemo()
{
    // The Lead track alone is armed
    int lead = -1;
    for (size_t i = 0; i < project().tracks.size(); ++i) {
        session_.store().editMix(static_cast<int>(i)).armed = false;
        if (project().tracks[i].name == "Lead")
            lead = static_cast<int>(i);
    }
    if (lead < 0)
        return;
    session_.store().editMix(lead).armed = true;
    session_.store().select(lead);
    // Straight in, with no count-in: the notes below are timed from now
    std::string why;
    recording_->start(recordingController_->takeFolder(), why);

    // An arpeggio, then a chord held on the sustain pedal: (seconds, message)
    struct Step {
        double at;
        audio::MidiMessage message;
    };
    auto steps = std::make_shared<std::vector<Step>>();
    const int keys[] = {60, 64, 67, 72, 67, 64};
    for (int i = 0; i < 6; ++i) {
        steps->push_back({0.3 + i * 0.25, audio::MidiMessage::noteOn(keys[i], 90 + i * 5)});
        steps->push_back({0.3 + i * 0.25 + 0.2, audio::MidiMessage::noteOff(keys[i])});
    }
    steps->push_back({2.0, {0xB0, 64, 127, 0}});
    for (int k : {57, 60, 64})
        steps->push_back({2.0, audio::MidiMessage::noteOn(k, 80)});
    for (int k : {57, 60, 64})
        steps->push_back({2.2, audio::MidiMessage::noteOff(k)});
    steps->push_back({3.0, {0xB0, 64, 0, 0}});
    const double start = ui_.window().time();
    auto next = std::make_shared<size_t>(0);
    demoTick_ = std::make_shared<std::function<void()>>();
    std::weak_ptr<std::function<void()>> weak = demoTick_;
    *demoTick_ = [this, start, steps, next, weak, lead] {
        const double now = ui_.window().time() - start;
        while (*next < steps->size() && (*steps)[*next].at <= now)
            midi_.send((*steps)[(*next)++].message);
        if (now < 3.4) {
            if (auto tick = weak.lock())
                ui_.post(*tick); // again next frame
            return;
        }
        // Done: stop, and open the take
        toggleRecording();
        audio_.transport().stop();
        const Track &t = project().tracks[static_cast<size_t>(lead)];
        if (!t.clips.empty())
            session_.store().select(lead, static_cast<int>(t.clips.size()) - 1);
        showEditor();
    };
    ui_.post(*demoTick_);
}

// Tracks and clips

void DawApp::deleteTrack(int index)
{
    const Track *t = project().track(index);
    if (!t)
        return;
    if (recording_->recording() || recording_->finishing()) {
        notify("Stop recording before deleting a track");
        return;
    }
    auto remove = [this, index] {
        // Changes the tree and the graph, so after this frame
        ui_.post([this, index] {
            if (!project().track(index))
                return;
            session_.store().edit().deleteTrack(index); // sync takes the track's processor out when its id is gone
        });
    };
    if (t->clips.empty()) {
        remove();
        return;
    }
    const size_t clips = t->clips.size();
    Dialog::open(gui_, "Delete \"" + t->name + "\"?",
                 format("Its %.0f clip", static_cast<double>(clips)) + (clips == 1 ? " goes" : "s go") + " with it.")
        .addButton("Cancel")
        .addButton("Delete track", remove, true);
}

void DawApp::splitSelectedClip()
{
    if (!project().clip()) {
        notify("Select a clip to split");
        return;
    }
    if (!session_.store().edit().splitClip(transportBeats(audio_)))
        notify("Put the playhead inside the selected clip to split it");
}

void DawApp::openClipMenu(Vec2 at, double beat)
{
    const Track *t = project().track(project().selectedTrack);
    const bool audio = t && t->kind == TrackKind::Audio;
    std::vector<MenuItem> items{
        MenuItem::item("Open in the editor", [this] { showEditor(); }),
        MenuItem::item("Split here", [this, beat] { session_.store().edit().splitClip(beat); }),
        MenuItem::item("Duplicate", [this] { session_.store().edit().duplicateClip(); }, "clip.duplicate"),
        MenuItem::item("Position and length...", [this] { openClipDialog(); }),
    };
    if (audio) {
        items.push_back(MenuItem::item("Remove fades", [this] {
            if (project().clip()) {
                Clip &c = session_.store().editClip(project().selectedTrack, project().selectedClip);
                c.fadeIn = c.fadeOut = 0.0;
            }
        }));
    }
    items.push_back(MenuItem::divider());
    items.push_back(MenuItem::item("Delete clip", [this] { session_.store().edit().deleteClip(); }));
    openContextMenu(gui_, items, at);
}

void DawApp::openClipDialog()
{
    const int track = project().selectedTrack;
    const int index = project().selectedClip;
    const Clip *clip = project().clip();
    if (!clip)
        return;
    // The clip this edits, looked up each time
    // (each use is an edit: the callers only ask for it when they are about to change the clip)
    auto target = [this, track, index]() -> Clip * {
        const Track *t = project().track(track);
        return t && index >= 0 && static_cast<size_t>(index) < t->clips.size() ? &session_.store().editClip(track, index) : nullptr;
    };
    const audio::BarPosition at = project().timing.position(clip->start);
    Dialog &dialog = Dialog::open(gui_, "Clip", "Where it starts on the timeline and how long it is.");
    dialog.setWidth(420.0f);
    Container &body = dialog.body();
    auto &name = labelled(body, "Name").add<TextField>("Clip name");
    name.setText(clip->name).setSize(Size::Small).setGrow(1.0f);
    name.onChange([target](const std::string &text) {
        if (Clip *c = target(); c && !text.empty())
            c->name = text;
    });
    // The start as a bar and a beat in it (from 1; a beat is the signature's unit), keeping any
    // fraction of a beat
    auto setStart = [this, target](double startBar, double beat) {
        if (Clip *c = target()) {
            const audio::TempoMap &timing = project().timing;
            const double fraction = timing.position(c->start).fraction;
            const int bar = std::max(0, static_cast<int>(startBar) - 1);
            const audio::Signature &s = timing.signatureAtBar(bar);
            const double unit = std::clamp(beat, 1.0, static_cast<double>(s.numerator)) - 1.0;
            const double start = timing.barStart(bar) + (unit + fraction) * s.unit();
            c->start = std::clamp(start, 0.0, std::max(0.0, project().lengthBeats() - c->length));
        }
    };
    auto &barStepper = labelled(body, "Bar").add<Stepper>(static_cast<float>(at.bar + 1), 1.0f,
                                                          static_cast<float>(project().bars), 1.0f);
    auto &beatStepper = labelled(body, "Beat").add<Stepper>(static_cast<float>(at.unit + 1), 1.0f, 16.0f, 1.0f);
    Stepper *barRaw = &barStepper, *beatRaw = &beatStepper;
    barStepper.onChange([setStart, beatRaw](float v) { setStart(v, beatRaw->value()); });
    beatStepper.onChange([setStart, barRaw](float v) { setStart(barRaw->value(), v); });
    barStepper.setSize(Size::Small);
    beatStepper.setSize(Size::Small);
    auto &length = labelled(body, "Length").add<Stepper>(static_cast<float>(clip->length), 1.0f, 1024.0f, 1.0f, [this, target](float beats) {
        if (Clip *c = target())
            c->length = std::min(static_cast<double>(beats), project().lengthBeats() - c->start);
    });
    length.setSuffix(" beats").setSize(Size::Small);
    dialog.addButton("Done", {}, true);
}

void DawApp::openAutomationMenu(int track, Vec2 at)
{
    const Track *t = project().track(track);
    if (!t)
        return;
    // Picking a parameter adds its lane (once) and shows the track's lanes; it is ticked once it has one
    auto add = [this, track](AutomationPlace place, uint32_t param) {
        return [this, track, place, param] {
            if (std::vector<audio::AutomationLane> *lanes = editLanesAt(session_.store(), track, place)) {
                if (!audio::findAutomation(*lanes, param))
                    lanes->push_back({param, {}});
            }
            arrangement_->showAutomation(track, true);
        };
    };
    auto has = [this, track](AutomationPlace place, uint32_t param) {
        return [this, track, place, param] {
            const std::vector<audio::AutomationLane> *lanes = lanesAt(project(), track, place);
            return lanes && audio::findAutomation(*lanes, param) != nullptr;
        };
    };
    std::vector<MenuItem> items;
    for (uint32_t param : {audio::kVolumeParam, audio::kPanParam})
        items.push_back(MenuItem::toggle(trackParam(param).name, has(AutomationPlace{}, param), add(AutomationPlace{}, param)));
    // Each send's level: the returns and groups it sends to or may (choosing one with no send makes it, at nothing)
    for (size_t r = 0; r < project().tracks.size(); ++r) {
        const Track &ret = project().tracks[r];
        if (!isBus(ret.kind) || (!findSend(*t, ret.id) && !canSendTo(project(), track, static_cast<int>(r))))
            continue;
        const ObjectId to = ret.id;
        auto addLane = add(AutomationPlace{}, sendParam(to));
        items.push_back(MenuItem::toggle("Send to " + ret.name, has(AutomationPlace{}, sendParam(to)), [this, track, to, addLane] {
            const Track *tr = project().track(track);
            if (tr && !findSend(*tr, to))
                session_.store().editMix(track).sends.push_back({to, 0.0f, false});
            addLane();
        }));
    }
    // Each device's parameters, in a submenu of its own (a plugin's once it has loaded)
    auto deviceMenu = [&](const DevicePlace &place, const audio::DeviceSetting &setting) {
        audio::Device *device = bridge_->deviceAt(project(), place);
        const AutomationPlace where{false, place};
        std::vector<MenuItem> params;
        if (device && device->automatable()) {
            for (const audio::DeviceParam &p : device->params()) {
                if (p.hidden || p.readOnly)
                    continue;
                const std::string label = p.module.empty() ? p.name : p.module + ": " + p.name;
                params.push_back(MenuItem::toggle(label, has(where, p.id), add(where, p.id)));
            }
        }
        if (params.empty()) {
            const std::string why = !device ? "Starting..." : device->automatable() ? "Loading..." : "Can't be automated";
            params.push_back(MenuItem::item(why, [] {}).when([] { return false; }));
        }
        items.push_back(MenuItem::sub(setting.name, std::move(params)));
    };
    if ((t->kind == TrackKind::Instrument && t->hasInstrument()) || !t->effects.empty())
        items.push_back(MenuItem::divider());
    if (t->kind == TrackKind::Instrument && t->hasInstrument())
        deviceMenu(DevicePlace::instrumentOf(track), t->instrument);
    for (size_t i = 0; i < t->effects.size(); ++i)
        deviceMenu(DevicePlace::effect(false, track, static_cast<int>(i)), t->effects[i]);
    openContextMenu(gui_, items, at);
}

void DawApp::openMasterAutomationMenu(Vec2 at)
{
    // Picking a parameter adds its lane (once) and shows the master's lanes; it is ticked once it has one
    auto add = [this](AutomationPlace place, uint32_t param) {
        return [this, place, param] {
            if (std::vector<audio::AutomationLane> *lanes = editLanesAt(session_.store(), -1, place)) {
                if (!audio::findAutomation(*lanes, param))
                    lanes->push_back({param, {}});
            }
            arrangement_->showMasterAutomation(true);
        };
    };
    auto has = [this](AutomationPlace place, uint32_t param) {
        return [this, place, param] {
            const std::vector<audio::AutomationLane> *lanes = lanesAt(project(), -1, place);
            return lanes && audio::findAutomation(*lanes, param) != nullptr;
        };
    };
    const AutomationPlace volume = AutomationPlace::masterVolume();
    std::vector<MenuItem> items{
        MenuItem::toggle(trackParam(audio::kVolumeParam).name, has(volume, audio::kVolumeParam), add(volume, audio::kVolumeParam))};
    if (!project().masterEffects.empty())
        items.push_back(MenuItem::divider());
    for (size_t i = 0; i < project().masterEffects.size(); ++i) {
        const DevicePlace place = DevicePlace::effect(true, 0, static_cast<int>(i));
        const AutomationPlace where = AutomationPlace::masterEffect(static_cast<int>(i));
        audio::Device *device = bridge_->deviceAt(project(), place);
        std::vector<MenuItem> params;
        if (device && device->automatable()) {
            for (const audio::DeviceParam &p : device->params()) {
                if (p.hidden || p.readOnly)
                    continue;
                const std::string label = p.module.empty() ? p.name : p.module + ": " + p.name;
                params.push_back(MenuItem::toggle(label, has(where, p.id), add(where, p.id)));
            }
        }
        if (params.empty()) {
            const std::string why = !device ? "Starting..." : device->automatable() ? "Loading..." : "Can't be automated";
            params.push_back(MenuItem::item(why, [] {}).when([] { return false; }));
        }
        items.push_back(MenuItem::sub(project().masterEffects[i].name, std::move(params)));
    }
    openContextMenu(gui_, items, at);
}

void DawApp::editTakes(int track, const std::function<bool(Track &)> &change)
{
    const Track *t = project().track(track);
    if (!t)
        return;
    Track copy = *t;
    if (!change(copy))
        return;
    Track &edited = session_.store().editTrack(track);
    edited.clips = std::move(copy.clips);
    edited.takeLanes = std::move(copy.takeLanes);
}

void DawApp::openTakeMenu(int track, ObjectId lane, ObjectId take, Vec2 at)
{
    const Track *t = project().track(track);
    const TakeLane *found = t ? findLane(*t, lane) : nullptr;
    if (!found)
        return;
    const ObjectId trackId = t->id;
    std::vector<MenuItem> items;
    if (take != kNoId) {
        items.push_back(MenuItem::item("Use this take", [this, track, take] {
            editTakes(track, [this, take](Track &tr) {
                const Clip *c = findTake(tr, take);
                return c && compRange(tr, c->start, c->end(), take, project().timing);
            });
        }));
    }
    items.push_back(MenuItem::item(take != kNoId ? "Use the whole lane" : "Use this lane",
                                   [this, track, lane] { editTakes(track, [this, lane](Track &tr) { return useLane(tr, lane, project().timing); }); }));
    items.push_back(MenuItem::toggle("Listen to this lane", [this, trackId, lane] { return bridge_->audition(trackId) == lane; },
                                     [this, trackId, lane] { bridge_->setAudition(trackId, bridge_->audition(trackId) == lane ? kNoId : lane); }));
    items.push_back(MenuItem::divider());
    items.push_back(MenuItem::item("Rename...", [this, track, lane] {
        const Track *tr = project().track(track);
        const TakeLane *l = tr ? findLane(*tr, lane) : nullptr;
        if (!l)
            return;
        Dialog &dialog = Dialog::open(gui_, "Rename take lane");
        dialog.setWidth(380.0f);
        auto &field = labelled(dialog.body(), "Name").add<TextField>("Take name");
        field.setText(l->name).setSize(Size::Small);
        field.onChange([this, track, lane](const std::string &text) {
            if (text.empty())
                return;
            editTakes(track, [lane, text](Track &tr) {
                TakeLane *edited = findLane(tr, lane);
                if (!edited || edited->name == text)
                    return false;
                edited->name = text;
                return true;
            });
        });
        field.onSubmit([&dialog](const std::string &) { dialog.close(); });
        field.focus();
        field.selectAll();
        dialog.addButton("Done", {}, true);
    }));
    // Its takes as ordinary clips on a track of their own, below this one
    items.push_back(MenuItem::item("Move to a new track", [this, track, lane] {
        const Track *tr = project().track(track);
        const TakeLane *l = tr ? findLane(*tr, lane) : nullptr;
        if (!l)
            return;
        Track fresh = *tr;
        fresh.id = kNoId;
        fresh.name = tr->name + " " + l->name;
        fresh.armed = false;
        fresh.solo = false;
        fresh.takeLanes.clear();
        fresh.clips = l->clips;
        for (Clip &c : fresh.clips) {
            c.id = kNoId;
            c.take = kNoId;
        }
        fresh.automation.clear();
        Project &p = session_.store().edit();
        p.tracks.insert(p.tracks.begin() + track + 1, std::move(fresh));
        p.select(track + 1);
    }));
    items.push_back(MenuItem::divider());
    items.push_back(MenuItem::item("Delete take lane", [this, track, trackId, lane] {
        if (bridge_->audition(trackId) == lane)
            bridge_->setAudition(trackId, kNoId);
        editTakes(track, [lane](Track &tr) { return removeLane(tr, lane); });
    }));
    openContextMenu(gui_, items, at);
}

void DawApp::openTrackMenu(int track, Vec2 at)
{
    const Track *t = project().track(track);
    if (!t)
        return;
    std::vector<MenuItem> items{MenuItem::item("Rename", [this, track] { arrangement_->renameTrack(track); })};
    if (!t->bus()) { // buses record nothing
        items.push_back(MenuItem::toggle("Armed for recording", [this, track] {
            const Track *tr = project().track(track);
            return tr && tr->armed;
        }, [this, track] {
            if (project().track(track)) {
                Track &tr = session_.store().editMix(track);
                tr.armed = !tr.armed;
            }
        }));
    }
    if (t->kind == TrackKind::Instrument) {
        // Its instrument: chosen in the device picker, opened (every control, and a plugin's own
        // interface), or removed
        items.push_back(MenuItem::item(t->hasInstrument() ? "Change instrument..." : "Choose instrument...", [this, track, at] {
            session_.store().select(track);
            pickDevice(true, false, track, {at.x, at.y, 1.0f, 1.0f});
        }));
        if (t->hasInstrument()) {
            items.push_back(MenuItem::item("Open " + t->instrument.name, [this, track] {
                session_.store().select(track);
                openDevice(DevicePlace::instrumentOf(track));
            }));
            items.push_back(MenuItem::item("Remove instrument", [this, track] {
                if (project().track(track))
                    session_.store().editTrack(track).instrument = {};
            }));
        }
        items.push_back(MenuItem::item("Add effect...", [this, track, at] {
            session_.store().select(track);
            pickDevice(false, false, track, {at.x, at.y, 1.0f, 1.0f});
        }));
    }
    if (t->kind == TrackKind::Audio) {
        // The input it records: none, a channel, or a stereo pair of the open input (two until one is open)
        const uint32_t channels = std::max<uint32_t>(2, audio_.inputChannels());
        std::vector<MenuItem> inputs;
        auto choice = [this, track](const std::string &label, int first, bool stereo) {
            return MenuItem::toggle(label, [this, track, first, stereo] {
                const Track *tr = project().track(track);
                return tr && tr->inputChannel == first && (first < 0 || tr->inputStereo == stereo);
            }, [this, track, first, stereo] {
                if (project().track(track)) {
                    Track &tr = session_.store().editMix(track);
                    tr.inputChannel = first;
                    tr.inputStereo = stereo;
                }
            });
        };
        inputs.push_back(choice("None", -1, false));
        for (uint32_t c = 0; c < channels; ++c)
            inputs.push_back(choice("In " + std::to_string(c + 1), static_cast<int>(c), false));
        for (uint32_t c = 0; c + 1 < channels; c += 2)
            inputs.push_back(choice("In " + std::to_string(c + 1) + "-" + std::to_string(c + 2) + " (stereo)", static_cast<int>(c), true));
        items.push_back(MenuItem::sub("Input", inputs));
        items.push_back(MenuItem::toggle("Hear the input while armed", [this, track] {
            const Track *tr = project().track(track);
            return tr && tr->monitor;
        }, [this, track] {
            if (project().track(track)) {
                Track &tr = session_.store().editMix(track);
                tr.monitor = !tr.monitor;
            }
        }));
        items.push_back(MenuItem::item("Import audio file here...", [this, track] { importAudio(track); }));
    }
    if (t->bus()) {
        items.push_back(MenuItem::item("Add effect...", [this, track, at] {
            session_.store().select(track);
            pickDevice(false, false, track, {at.x, at.y, 1.0f, 1.0f});
        }));
    }
    // Where it goes: the master or a group (none that would loop), and a new group around it
    std::vector<MenuItem> outputs;
    auto output = [this, track](const std::string &label, ObjectId id) {
        return MenuItem::toggle(label, [this, track, id] {
            const Track *tr = project().track(track);
            return tr && tr->output == id;
        }, [this, track, id] {
            const Track *tr = project().track(track);
            if (tr && tr->output != id)
                session_.store().editMix(track).output = id;
        });
    };
    outputs.push_back(output("Master", kNoId));
    for (int g : outputChoices(project(), track))
        outputs.push_back(output(project().tracks[static_cast<size_t>(g)].name, project().tracks[static_cast<size_t>(g)].id));
    items.push_back(MenuItem::divider());
    // Also to: a send at 0 dB into each other group it may feed (not its output group; that would play twice)
    const std::vector<int> also = alsoToChoices(project(), track);
    if (!also.empty()) {
        outputs.push_back(MenuItem::divider());
        outputs.push_back(MenuItem::item("Also to", [] {}).when([] { return false; }));
        for (int g : also) {
            const ObjectId id = project().tracks[static_cast<size_t>(g)].id;
            outputs.push_back(MenuItem::toggle(project().tracks[static_cast<size_t>(g)].name, [this, track, id] {
                const Track *tr = project().track(track);
                return tr && findSend(*tr, id) != nullptr;
            }, [this, track, id] {
                const Track *tr = project().track(track);
                setAlsoTo(session_.store(), track, id, tr && !findSend(*tr, id));
            }));
        }
    }
    items.push_back(MenuItem::sub("Output", outputs));
    items.push_back(MenuItem::item("Group the track", [this, track] { groupTrack(track); }, "track.group"));
    // Its takes: shown or not, the comp made final, takes the comp doesn't use cleared away
    if (!t->takeLanes.empty()) {
        items.push_back(MenuItem::divider());
        items.push_back(MenuItem::toggle("Take lanes", [this, track] { return arrangement_->takesShown(track); },
                                         [this, track] { arrangement_->showTakes(track, !arrangement_->takesShown(track)); }));
        items.push_back(MenuItem::item("Delete unused takes", [this, track] { editTakes(track, [](Track &tr) { return deleteUnusedTakes(tr); }); }));
        items.push_back(MenuItem::item("Flatten comp", [this, track] {
            bridge_->setAudition(project().tracks[static_cast<size_t>(track)].id, kNoId);
            editTakes(track, [](Track &tr) { return flattenComp(tr); });
        }));
    }
    items.push_back(MenuItem::divider());
    items.push_back(MenuItem::item("Delete track", [this, track] { deleteTrack(track); }));
    openContextMenu(gui_, items, at);
}

// Settings for inputs

void DawApp::buildPerformanceSettings(Container &body)
{
    env::Scheduler *scheduler = env_.find<env::Scheduler>();
    audio::MemoryManager *memory = env_.find<audio::MemoryManager>();
    body.add<Label>("Tracks are mixed on several cores at once. Long samples keep their first moments in memory "
                    "and stream the rest from a cache on disk, so an instrument takes a fraction of the memory.",
                    TextStyle::Caption, TextTone::Secondary)
        .setWrapWidth(500.0f);
    auto save = [this] {
        std::string error;
        settings_.save(error);
    };

    if (scheduler) {
        // Auto, the audio thread alone, then counts up to the pool's size
        const uint32_t pool = scheduler->realtime().workers();
        std::vector<int> counts{-1, 0};
        for (uint32_t n : {1u, 2u, 3u, 4u, 6u, 8u, 12u, 15u}) {
            if (n <= pool)
                counts.push_back(static_cast<int>(n));
        }
        std::vector<std::string> names;
        int current = 0;
        for (size_t i = 0; i < counts.size(); ++i) {
            const int n = counts[i];
            names.push_back(n < 0 ? format("Automatic (%.0f)", pool + 1.0) : n == 0 ? std::string("1 (the audio thread alone)")
                                                                                     : format("%.0f", n + 1.0));
            if (n == settings_.audioThreads)
                current = static_cast<int>(i);
        }
        labelled(body, "Mixing threads").add<Dropdown>(gui_, names, current, [this, scheduler, counts, pool, save](int i) {
            const int n = counts[static_cast<size_t>(i)];
            settings_.audioThreads = n;
            scheduler->setRealtimeWorkers(n < 0 ? pool : static_cast<uint32_t>(n));
            save();
        }).setSize(Size::Small).setGrow(1.0f);
    }

    if (memory) {
        body.add<Checkbox>("Stream long samples from disk (for instruments loaded from now on)", settings_.streamSamples,
                           [this, memory, save](bool on) {
                               settings_.streamSamples = on;
                               audio::MemoryConfig config = memory->config();
                               config.streaming = on;
                               memory->configure(config);
                               save();
                           })
            .setSize(Size::Small);
        const std::vector<uint32_t> budgets{64, 128, 256, 512, 1024, 2048};
        std::vector<std::string> names;
        int current = 1;
        for (size_t i = 0; i < budgets.size(); ++i) {
            names.push_back(format("%.0f MB", budgets[i]));
            if (budgets[i] == settings_.streamMemoryMB)
                current = static_cast<int>(i);
        }
        labelled(body, "Stream memory").add<Dropdown>(gui_, names, current, [this, memory, budgets, save](int i) {
            settings_.streamMemoryMB = budgets[static_cast<size_t>(i)];
            audio::MemoryConfig config = memory->config();
            config.budgetBytes = static_cast<uint64_t>(settings_.streamMemoryMB) << 20;
            memory->configure(config);
            save();
        }).setSize(Size::Small).setGrow(1.0f);
        body.add<Checkbox>("Let overlays hook in (Steam, OBS, Discord; next start, more memory)",
                           settings_.graphicsOverlays,
                           [this, save](bool on) {
                               settings_.graphicsOverlays = on;
                               save();
                           })
            .setSize(Size::Small);
        body.add<Checkbox>("Use Windows' own file dialogs (slower to open)", settings_.nativeFileDialogs,
                           [this, save](bool on) {
                               settings_.nativeFileDialogs = on;
                               save();
                           })
            .setSize(Size::Small);
        if (env::ResourceManager *resources = env_.find<env::ResourceManager>()) {
            const std::vector<int> times{10, 30, 60, 300, -1};
            const std::vector<std::string> names{"10 seconds", "30 seconds", "1 minute", "5 minutes", "Until VYBN closes"};
            int current = 1;
            for (size_t i = 0; i < times.size(); ++i) {
                if (times[i] == settings_.unusedSeconds)
                    current = static_cast<int>(i);
            }
            labelled(body, "Keep unused sounds").add<Dropdown>(gui_, names, current, [this, resources, times, save](int i) {
                settings_.unusedSeconds = times[static_cast<size_t>(i)];
                env::ResourceConfig config = resources->config();
                config.idleSeconds = settings_.unusedSeconds < 0 ? 1e12 : settings_.unusedSeconds;
                resources->configure(config);
                save();
            }).setSize(Size::Small).setGrow(1.0f);
        }
        // Its own row, so the statistics line doesn't widen the whole dialog
        auto &cache = line(body);
        cache.add<Spacer>();
        cache.add<Button>([memory] {
            const std::string folder = memory->config().cacheFolder;
            std::error_code ec;
            fs::create_directories(fs::u8path(folder), ec);
            engine::showInFolder(folder);
        }).setLabel("Sample cache").setVariant(ButtonVariant::Flat).setSize(Size::Small);
    }
}

void DawApp::buildMetrics(Container &body)
{
    env::Scheduler *scheduler = env_.find<env::Scheduler>();
    audio::MemoryManager *memory = env_.find<audio::MemoryManager>();
    audio::AudioSystem &audio = audio_;
    auto heading = [&body](const char *text) { body.add<Label>(text, TextStyle::Body); };
    auto figure = [&body](std::function<std::string()> text) {
        body.add<LiveLabel>(std::move(text), TextStyle::Caption, TextTone::Secondary);
    };

    heading("Audio");
    figure([&audio] { return audio.running() ? audio.deviceName() : "No audio device: " + audio.lastError(); });
    figure([&audio] {
        if (!audio.running())
            return std::string();
        return format("%.0f Hz", audio.sampleRate()) + format(",  %.0f frames", audio.bufferFrames()) +
               format(" (%.1f ms)", audio.bufferMs()) + ",  " + audio.backendName() + (audio.exclusive() ? " exclusive" : " shared");
    });
    auto &load = labelled(body, "Load");
    ProgressBar *bar = &load.add<ProgressBar>(0.0f);
    bar->setSize(Size::Small).setGrow(1.0f);
    load.add<LiveLabel>([&audio, bar] {
        bar->setValue(audio.load());
        return format("%.1f %%", audio.load() * 100.0);
    }, TextStyle::Caption).setMinSize({52.0f, 0.0f});
    figure([&audio] {
        return format("%.0f callbacks", static_cast<double>(audio.callbacks())) +
               format(",  %.0f ran over their time", static_cast<double>(audio.overloads()));
    });

    if (scheduler) {
        heading("Threads");
        figure([scheduler] {
            const uint32_t threads = scheduler->realtime().active() + 1;
            return format("Mixing on %.0f thread", threads) + (threads == 1 ? "" : "s") +
                   format(" (%.0f in the pool)", scheduler->realtime().workers() + 1.0);
        });
        figure([scheduler] {
            std::string text;
            for (env::Lane lane : {env::Lane::Interactive, env::Lane::Background, env::Lane::Disk, env::Lane::Network}) {
                const env::Scheduler::LaneStats s = scheduler->stats(lane);
                text += (text.empty() ? "" : ",   ") + std::string(env::laneName(lane)) + format(" %.0f done", static_cast<double>(s.completed));
                if (s.queued > 0)
                    text += format(" (%.0f waiting)", static_cast<double>(s.queued));
            }
            return "Jobs:  " + text;
        });
    }

    if (memory) {
        heading("Sample memory");
        figure([memory] {
            const audio::MemoryManager::Stats m = memory->stats();
            return format("%.1f MB preloaded", m.preloadBytes / 1048576.0) + format(",  %.1f", m.poolBytes / 1048576.0) +
                   format(" of %.0f MB of streamed chunks", m.budgetBytes / 1048576.0) +
                   format(",  %.0f samples streaming", m.sources);
        });
        figure([memory] {
            const audio::MemoryManager::Stats m = memory->stats();
            return format("%.0f chunks read", static_cast<double>(m.reads)) + format(",  %.0f evicted", static_cast<double>(m.evictions)) +
                   format(",  %.0f dropouts", static_cast<double>(m.underruns));
        });
    }
    if (env::ResourceManager *resources = env_.find<env::ResourceManager>()) {
        heading("Loaded");
        figure([] { return format("The app:  %.0f MB", env::processMemory() / 1048576.0); });
        for (size_t i = 0; i < resources->stats().size(); ++i) {
            figure([resources, i] {
                const auto all = resources->stats();
                if (i >= all.size())
                    return std::string();
                const env::ResourceManager::PoolStats &p = all[i];
                return p.name + format(":  %.0f", p.usage.items) + format(" (%.1f MB)", p.usage.bytes / 1048576.0) +
                       format(",  %.0f unused", p.usage.idleItems) + format(" (%.1f MB)", p.usage.idleBytes / 1048576.0) +
                       format(",  %.1f MB let go", p.freed / 1048576.0);
            });
        }
        // What nothing uses goes after the time set in Performance; this lets it go now
        auto &row = line(body);
        row.add<Spacer>();
        row.add<Button>([resources] { resources->trim(); })
            .setLabel("Free unused now")
            .setVariant(ButtonVariant::Flat)
            .setSize(Size::Small);
    }
    figure([this] {
        const std::string status = bridge_->instrumentStatus(project(), project().selectedTrack);
        const Track *t = project().track(project().selectedTrack);
        if (!t || t->kind != TrackKind::Instrument || t->instrument.kind != audio::DeviceKind::Sampler)
            return std::string("Select a track playing a library instrument to see its memory");
        return t->instrument.name + ":  " + (status.empty() ? std::string("loading") : status);
    });

    // Frames the window drew, and those skipped because the picture was unchanged
    figure([this] {
        const ui::UISystem::FrameRate rate = ui_.frameRate();
        return "UI frames:  " + format("%.0f drawn", rate.drawn) + format(",  %.0f skipped per second", rate.skipped) +
               ",  loop " + ui_.paceText();
    });
    // What the window drew: draws, submit time and upload size are only recorded on presented
    // frames, so the last presented values are kept while frames are skipped as unchanged
    figure([this, draws = uint32_t(0), submitMs = 0.0, bytes = size_t(0)]() mutable {
        const ui::FrameStats &s = ui_.frameStats();
        if (s.presented) {
            draws = s.draws;
            submitMs = s.submitMs;
            bytes = s.uploadBytes;
        }
        const uint32_t prims = s.surfaces + s.glyphs + s.images + s.lines + s.plots;
        return "UI drawing:  " + format("%.0f prims", static_cast<double>(prims)) +
               format(" (%.0f glyphs", static_cast<double>(s.glyphs)) +
               format(", %.0f plots)", static_cast<double>(s.plots)) +
               format(",  %.0f", static_cast<double>(draws)) + (draws == 1 ? " draw" : " draws") +
               format(",  %.0f KB", static_cast<double>(bytes) / 1024.0) +
               format(";  build %.2f ms", s.buildMs) + format(", submit %.2f ms", submitMs);
    });
    // The draw caches in the last frame, and how often the layout was skipped over the last second
    figure([this, shown = -1.0, frames = uint32_t(0), skipped = uint32_t(0), started = false,
            since = std::chrono::steady_clock::time_point()]() mutable {
        const ui::FrameStats &s = ui_.frameStats();
        const auto now = std::chrono::steady_clock::now();
        if (!started) {
            started = true;
            since = now;
        }
        ++frames;
        if (s.layoutSkipped)
            ++skipped;
        if (std::chrono::duration<double>(now - since).count() >= 1.0) {
            shown = 100.0 * skipped / frames;
            frames = 0;
            skipped = 0;
            since = now;
        }
        const double share = shown >= 0.0 ? shown : 100.0 * skipped / frames;
        // Thousands separators, as in "1,450"
        std::string digits = std::to_string(s.cachedPrims), prims;
        for (size_t i = 0; i < digits.size(); ++i) {
            if (i > 0 && (digits.size() - i) % 3 == 0)
                prims += ',';
            prims += digits[i];
        }
        return "UI caches:  " + format("%.0f hit", static_cast<double>(s.cacheHits)) +
               format(", %.0f missed", static_cast<double>(s.cacheMisses)) +
               " (" + prims + " prims reused);" +
               format(" layout skipped on %.0f%%", share) + " of frames";
    });

    // A line per daemon (one line for all would widen the dialog)
    heading("Main loop, busy per frame");
    for (size_t i = 0; i < env_.stats().size(); ++i) {
        figure([this, i] {
            const auto &all = env_.stats();
            if (i >= all.size() || !all[i].daemon)
                return std::string();
            const env::DaemonStats &s = all[i];
            return s.daemon->name() + format(":  %.2f ms", s.averageMs) + format(",  peak %.2f ms", s.peakMs) +
                   (s.waitMs > 0.05 ? format(",  waits %.1f ms", s.waitMs) : std::string());
        });
    }
}

void DawApp::buildInputSettings(Container &body)
{
    audio::AudioSystem &system = audio_;
    // The audio input: None, the system default, or a device
    std::vector<std::string> inputs{"None", "System default"};
    int current = settings_.audioInput == Settings::kNoInput ? 0 : 1;
    for (const audio::DeviceInfo &d : system.inputDevices()) {
        inputs.push_back(d.isDefault ? d.name + " (default)" : d.name);
        if (d.name == settings_.audioInput)
            current = static_cast<int>(inputs.size()) - 1;
    }
    auto &input = labelled(body, "Audio input");
    input.add<Dropdown>(gui_, inputs, current, [this](int i) {
        const auto &devices = audio_.inputDevices();
        settings_.audioInput = i == 0 ? std::string(Settings::kNoInput)
                               : i == 1 ? std::string()
                                        : devices[static_cast<size_t>(i - 2)].name;
        std::string error;
        settings_.save(error);
        inputTried_ = false;
        openAudioInput();
    }).setSize(Size::Small).setGrow(1.0f);

    // A meter per input channel
    auto &meters = labelled(body, "Levels");
    std::vector<LevelMeter *> bars;
    for (uint32_t c = 0; c < audio::AudioSystem::kMaxInputs; ++c) {
        auto &m = meters.add<LevelMeter>(Direction::Column);
        m.setThickness(6.0f).setLength(44.0f);
        bars.push_back(&m);
    }
    meters.add<LiveLabel>([&system] {
        return system.inputChannels() > 0 ? system.inputName() : std::string("No input open");
    }, TextStyle::Caption, TextTone::Secondary);
    body.add<Ticker>([this, bars, &system] {
        for (uint32_t c = 0; c < bars.size(); ++c) {
            bars[c]->setEnabled(c < system.inputChannels());
            bars[c]->setLevel(c < system.inputChannels() ? inputPeaks_[c] : 0.0f);
        }
    });
    body.add<Label>("An audio track records the channel (or pair) chosen in its inspector. Tick \"Hear the input\" "
                    "to monitor it while armed; use headphones, or a microphone will feed back.",
                    TextStyle::Caption, TextTone::Muted)
        .setWrapWidth(560.0f);

    // MIDI inputs: every one plugged in, each on or off
    body.add<Divider>();
    body.add<Label>("MIDI inputs", TextStyle::Subtitle);
    const auto &names = midi_.devices();
    if (names.empty())
        body.add<Label>("No MIDI keyboards or controllers are plugged in. They appear here when they are.",
                        TextStyle::Caption, TextTone::Secondary);
    for (size_t i = 0; i < names.size(); ++i) {
        auto &device = line(body);
        auto last = std::make_shared<uint64_t>(midi_.activity(i));
        auto lit = std::make_shared<double>(-10.0);
        device.add<ActivityLight>([this, i, last, lit] {
            const uint64_t now = midi_.activity(i);
            if (now != *last) {
                *last = now;
                *lit = ui_.window().time();
            }
            return ui_.window().time() - *lit < 0.12;
        });
        const std::string name = names[i];
        const bool on = std::find(settings_.midiDisabled.begin(), settings_.midiDisabled.end(), name) ==
                        settings_.midiDisabled.end();
        device.add<Checkbox>(name, on, [this, name](bool enabled) {
            auto &off = settings_.midiDisabled;
            off.erase(std::remove(off.begin(), off.end(), name), off.end());
            if (!enabled)
                off.push_back(name);
            std::string error;
            settings_.save(error);
            midiFailed_.erase(name);
            syncInputs(true);
        }).setSize(Size::Small);
        device.add<Spacer>();
        device.add<LiveLabel>([this, i] { return midi_.isOpen(i) ? std::string("In use") : std::string("Off"); },
                              TextStyle::Caption, TextTone::Secondary);
    }
    body.add<Label>("Armed instrument tracks play what comes in (or the selected one, when none is armed).",
                    TextStyle::Caption, TextTone::Muted)
        .setWrapWidth(560.0f);
}

// Project dialogs

namespace {

// A time signature's denominators, as the dialogs offer them
const int kBeatUnits[] = {2, 4, 8, 16};
const std::vector<std::string> kBeatUnitNames = {"Half note (/2)", "Quarter note (/4)", "Eighth note (/8)", "Sixteenth note (/16)"};

int beatUnitIndex(int denominator)
{
    for (int i = 0; i < 4; ++i) {
        if (kBeatUnits[i] == denominator)
            return i;
    }
    return 1;
}

}

// The tempo map's menus and editors (from the arrangement's tempo and signature lanes, and the
// transport bar)

namespace {

// The signatures offered with one click
const audio::Signature kCommonSignatures[] = {{0, 2, 4}, {0, 3, 4}, {0, 4, 4}, {0, 5, 4}, {0, 6, 8}, {0, 7, 8}, {0, 9, 8}, {0, 12, 8}};

std::string signatureName(int numerator, int denominator)
{
    return std::to_string(numerator) + "/" + std::to_string(denominator);
}

}

void DawApp::setTempoAt(double beat, double bpm)
{
    audio::TempoMap timing = project().timing;
    std::vector<audio::TempoPoint> points = timing.points();
    points[timing.pointAt(beat)].bpm = bpm;
    timing.setPoints(std::move(points));
    session_.store().setTiming(timing);
}

void DawApp::setSignatureAt(int bar, int numerator, int denominator)
{
    audio::TempoMap timing = project().timing;
    std::vector<audio::Signature> signatures = timing.signatures();
    auto at = std::find_if(signatures.begin(), signatures.end(), [bar](const audio::Signature &s) { return s.bar == bar; });
    if (at != signatures.end()) {
        at->numerator = numerator;
        at->denominator = denominator;
    } else {
        signatures.push_back({bar, numerator, denominator});
    }
    timing.setSignatures(std::move(signatures));
    session_.store().setTiming(timing);
}

void DawApp::openTempoMenu(int point, Vec2 at)
{
    const auto &points = project().timing.points();
    if (point < 0 || static_cast<size_t>(point) >= points.size())
        return;
    // Each edit finds the point again by its beat, in case the map changed while the menu was open
    const double beat = points[static_cast<size_t>(point)].beat;
    auto change = [this, beat](auto &&f) {
        audio::TempoMap timing = project().timing;
        std::vector<audio::TempoPoint> edited = timing.points();
        const size_t i = timing.pointAt(beat);
        if (edited[i].beat != beat)
            return;
        f(edited, i);
        timing.setPoints(std::move(edited));
        session_.store().setTiming(timing);
    };
    const bool last = static_cast<size_t>(point) + 1 == points.size();
    std::vector<MenuItem> items{
        MenuItem::toggle("Ramp to the next point", [this, beat] {
            const audio::TempoMap &timing = project().timing;
            return timing.points()[timing.pointAt(beat)].ramp;
        }, [change] { change([](std::vector<audio::TempoPoint> &p, size_t i) { p[i].ramp = !p[i].ramp; }); }).when([last] { return !last; }),
        MenuItem::item("Set the tempo...", [this, point] { openTempoDialog(point); }),
        MenuItem::item("Half the tempo", [change] { change([](std::vector<audio::TempoPoint> &p, size_t i) { p[i].bpm /= 2.0; }); }),
        MenuItem::item("Double the tempo", [change] { change([](std::vector<audio::TempoPoint> &p, size_t i) { p[i].bpm *= 2.0; }); }),
        MenuItem::divider(),
        MenuItem::item("Move the playhead here", [this, beat] { seekBeats(audio_, beat); }),
    };
    if (point > 0) {
        items.push_back(MenuItem::divider());
        items.push_back(MenuItem::item("Delete point", [change] {
            change([](std::vector<audio::TempoPoint> &p, size_t i) { p.erase(p.begin() + static_cast<std::ptrdiff_t>(i)); });
        }, "edit.delete"));
    }
    openContextMenu(gui_, items, at);
}

void DawApp::openTempoDialog(int point)
{
    const auto &points = project().timing.points();
    if (point < 0 || static_cast<size_t>(point) >= points.size())
        return;
    const audio::TempoPoint now = points[static_cast<size_t>(point)];
    // Typed: a number from 20 to 400 changes the point as it is typed; anything else waits
    auto apply = [this, beat = now.beat](const std::string &text) {
        char *end = nullptr;
        const double bpm = std::strtod(text.c_str(), &end);
        if (end == text.c_str() || bpm < audio::TempoMap::kMinBpm || bpm > audio::TempoMap::kMaxBpm)
            return;
        const audio::TempoMap &timing = project().timing;
        if (timing.points()[timing.pointAt(beat)].beat == beat)
            setTempoAt(beat, std::round(bpm * 100.0) / 100.0);
    };
    Dialog &dialog = Dialog::open(gui_, "Tempo", "The tempo from " + barsBeats(now.beat, project().timing) +
                                                     ". Type a number from 20 to 400.");
    dialog.setWidth(380.0f);
    Container &body = dialog.body();
    auto &field = labelled(body, "BPM").add<TextField>("120");
    field.setText(format("%g", now.bpm)).setSize(Size::Small);
    field.onChange(apply);
    field.onSubmit([apply, &dialog](const std::string &text) {
        apply(text);
        dialog.close();
    });
    field.focus();
    field.selectAll();
    if (static_cast<size_t>(point) + 1 < points.size()) {
        body.add<Checkbox>("Ramp to the next point", now.ramp, [this, beat = now.beat](bool on) {
            audio::TempoMap timing = project().timing;
            std::vector<audio::TempoPoint> edited = timing.points();
            const size_t i = timing.pointAt(beat);
            if (edited[i].beat != beat)
                return;
            edited[i].ramp = on;
            timing.setPoints(std::move(edited));
            session_.store().setTiming(timing);
        }).setSize(Size::Small);
    }
    dialog.addButton("Done", {}, true);
}

void DawApp::openSignatureMenu(int signature, Vec2 at)
{
    const auto &signatures = project().timing.signatures();
    if (signature < 0 || static_cast<size_t>(signature) >= signatures.size())
        return;
    const int bar = signatures[static_cast<size_t>(signature)].bar;
    std::vector<MenuItem> items;
    // The common signatures, ticked when it is one
    for (const audio::Signature &s : kCommonSignatures) {
        items.push_back(MenuItem::toggle(signatureName(s.numerator, s.denominator), [this, bar, s] {
            const audio::Signature &now = project().timing.signatureAtBar(bar);
            return now.numerator == s.numerator && now.denominator == s.denominator;
        }, [this, bar, s] { setSignatureAt(bar, s.numerator, s.denominator); }));
    }
    items.push_back(MenuItem::item("Other...", [this, signature] { openSignatureDialog(signature); }));
    items.push_back(MenuItem::divider());
    items.push_back(MenuItem::item("Move the playhead here", [this, bar] { seekBeats(audio_, project().timing.barStart(bar)); }));
    if (signature > 0) {
        items.push_back(MenuItem::divider());
        items.push_back(MenuItem::item("Delete change", [this, bar] {
            audio::TempoMap timing = project().timing;
            std::vector<audio::Signature> edited = timing.signatures();
            edited.erase(std::remove_if(edited.begin(), edited.end(), [bar](const audio::Signature &s) { return s.bar == bar; }),
                         edited.end());
            timing.setSignatures(std::move(edited));
            session_.store().setTiming(timing);
        }, "edit.delete"));
    }
    openContextMenu(gui_, items, at);
}

void DawApp::openSignatureDialog(int signature)
{
    const auto &signatures = project().timing.signatures();
    if (signature < 0 || static_cast<size_t>(signature) >= signatures.size())
        return;
    const audio::Signature now = signatures[static_cast<size_t>(signature)];
    auto current = [this, bar = now.bar] { return project().timing.signatureAtBar(bar); };
    auto change = [this, bar = now.bar](int numerator, int denominator) { setSignatureAt(bar, numerator, denominator); };
    Dialog &dialog = Dialog::open(gui_, "Time signature", "From bar " + std::to_string(now.bar + 1) + " on.");
    dialog.setWidth(420.0f);
    Container &body = dialog.body();
    auto &beats = labelled(body, "Beats per bar")
                      .add<Stepper>(static_cast<float>(now.numerator), 1.0f, 16.0f, 1.0f,
                                    [change, current](float n) { change(static_cast<int>(n), current().denominator); });
    beats.setSize(Size::Small);
    auto &unit = labelled(body, "Beat unit")
                     .add<Dropdown>(gui_, kBeatUnitNames, beatUnitIndex(now.denominator),
                                    [change, current](int i) { change(current().numerator, kBeatUnits[i]); });
    unit.setSize(Size::Small);
    // The common ones in one click (the controls above follow)
    auto &common = labelled(body, "Common").add<Container>(Direction::Row);
    for (const audio::Signature &s : kCommonSignatures) {
        Stepper *stepper = &beats;
        Dropdown *dropdown = &unit;
        common.add<Button>([change, s, stepper, dropdown] {
            change(s.numerator, s.denominator);
            stepper->setValue(static_cast<float>(s.numerator));
            dropdown->setSelected(beatUnitIndex(s.denominator));
        }).setLabel(signatureName(s.numerator, s.denominator)).setSize(Size::Small);
    }
    dialog.addButton("Done", {}, true);
}

void DawApp::openTimingLaneMenu(Vec2 at, double beat, bool tempo)
{
    std::vector<MenuItem> items;
    if (tempo) {
        items.push_back(MenuItem::item("Add a tempo change here", [this, beat] {
            audio::TempoMap timing = project().timing;
            std::vector<audio::TempoPoint> points = timing.points();
            if (std::none_of(points.begin(), points.end(), [beat](const audio::TempoPoint &p) { return p.beat == beat; }))
                points.push_back({beat, timing.bpmAt(beat), false});
            timing.setPoints(std::move(points));
            session_.store().setTiming(timing);
        }).when([beat] { return beat > 0.0; }));
        items.push_back(MenuItem::item("One tempo for the whole song", [this] {
            audio::TempoMap timing = project().timing;
            timing.setPoints({timing.points().front()});
            std::vector<audio::TempoPoint> one = timing.points();
            one.front().ramp = false;
            timing.setPoints(std::move(one));
            session_.store().setTiming(timing);
        }).when([this] { return project().timing.points().size() > 1; }));
    } else {
        const int bar = project().timing.barAt(beat);
        // A change at this bar, as one of the common signatures
        for (const audio::Signature &s : kCommonSignatures) {
            items.push_back(MenuItem::item("From bar " + std::to_string(bar + 1) + ": " + signatureName(s.numerator, s.denominator),
                                           [this, bar, s] { setSignatureAt(bar, s.numerator, s.denominator); })
                                .when([bar] { return bar > 0; }));
        }
        items.push_back(MenuItem::item("One signature for the whole song", [this] {
            audio::TempoMap timing = project().timing;
            timing.setSignatures({timing.firstSignature()});
            session_.store().setTiming(timing);
        }).when([this] { return project().timing.signatures().size() > 1; }));
    }
    items.push_back(MenuItem::divider());
    items.push_back(MenuItem::item("Hide the tempo and signature lanes", [this] { arrangement_->showTiming(false); }, "view.timing"));
    openContextMenu(gui_, items, at);
}

void DawApp::openProjectSettings()
{
    Dialog &dialog = Dialog::open(gui_, "Project settings");
    dialog.setWidth(460.0f);
    Container &body = dialog.body();
    auto &name = labelled(body, "Name").add<TextField>("Project name");
    name.setText(project().name);
    name.setSize(Size::Small).setGrow(1.0f);
    name.onChange([this](const std::string &text) { session_.store().edit().name = text; });
    // The opening tempo and time signature (later changes are on the arrangement's tempo and
    // signature lanes)
    labelled(body, "Tempo")
        .add<Stepper>(static_cast<float>(project().timing.tempo()), 20.0f, 400.0f, 1.0f, [this](float bpm) {
            audio::TempoMap timing = project().timing;
            timing.setTempo(bpm);
            session_.store().setTiming(timing);
        })
        .setSuffix(" BPM")
        .setSize(Size::Small);
    auto setSignature = [this](int numerator, int denominator) {
        audio::TempoMap timing = project().timing;
        std::vector<audio::Signature> signatures = timing.signatures();
        signatures.front().numerator = numerator;
        signatures.front().denominator = denominator;
        timing.setSignatures(std::move(signatures));
        session_.store().setTiming(timing);
    };
    labelled(body, "Beats per bar")
        .add<Stepper>(static_cast<float>(project().timing.firstSignature().numerator), 1.0f, 16.0f, 1.0f,
                      [this, setSignature](float beats) { setSignature(static_cast<int>(beats), project().timing.firstSignature().denominator); })
        .setSize(Size::Small);
    labelled(body, "Beat unit")
        .add<Dropdown>(gui_, kBeatUnitNames, beatUnitIndex(project().timing.firstSignature().denominator),
                       [this, setSignature](int i) { setSignature(project().timing.firstSignature().numerator, kBeatUnits[i]); })
        .setSize(Size::Small);
    labelled(body, "Length")
        .add<Stepper>(static_cast<float>(project().bars), 4.0f, 999.0f, 4.0f,
                      [this](float bars) { session_.store().edit().bars = static_cast<int>(bars); })
        .setSuffix(" bars")
        .setSize(Size::Small);
    body.add<LiveLabel>([this] {
        size_t clips = 0;
        for (const Track &t : project().tracks)
            clips += t.clips.size();
        return format("%.0f tracks", static_cast<double>(project().tracks.size())) +
               format(", %.0f clips", static_cast<double>(clips)) +
               (session_.path().empty() ? std::string(",  not saved yet") : ",  " + session_.path());
    }, TextStyle::Caption, TextTone::Muted).setWrapWidth(420.0f);
    dialog.addButton("Done", {}, true);
}

void DawApp::openNewProjectDialog()
{
    auto options = std::make_shared<NewProjectOptions>();
    Dialog &dialog = Dialog::open(gui_, "New project", "Set it up now; everything can be changed later in Project settings.");
    dialog.setWidth(460.0f);
    Container &body = dialog.body();
    auto &name = labelled(body, "Name").add<TextField>("Project name");
    name.setText(options->name);
    name.setSize(Size::Small).setGrow(1.0f);
    name.onChange([options](const std::string &text) { options->name = text; });
    labelled(body, "Tempo")
        .add<Stepper>(static_cast<float>(options->tempo), 20.0f, 400.0f, 1.0f,
                      [options](float bpm) { options->tempo = bpm; })
        .setSuffix(" BPM")
        .setSize(Size::Small);
    labelled(body, "Beats per bar")
        .add<Stepper>(static_cast<float>(options->beatsPerBar), 1.0f, 16.0f, 1.0f,
                      [options](float beats) { options->beatsPerBar = static_cast<int>(beats); })
        .setSize(Size::Small);
    labelled(body, "Beat unit")
        .add<Dropdown>(gui_, kBeatUnitNames, beatUnitIndex(options->beatUnit), [options](int i) { options->beatUnit = kBeatUnits[i]; })
        .setSize(Size::Small);
    labelled(body, "Length")
        .add<Stepper>(static_cast<float>(options->bars), 4.0f, 999.0f, 4.0f,
                      [options](float bars) { options->bars = static_cast<int>(bars); })
        .setSuffix(" bars")
        .setSize(Size::Small);
    body.add<Checkbox>("Start with an instrument track", options->instrumentTrack,
                       [options](bool on) { options->instrumentTrack = on; })
        .setSize(Size::Small);
    body.add<Checkbox>("Start with an audio track", options->audioTrack, [options](bool on) { options->audioTrack = on; })
        .setSize(Size::Small);
    dialog.addButton("Cancel");
    dialog.addButton("Create", [this, options] {
        const auto &names = library_->instruments();
        // The guitar the demo uses if it's installed, else the library's first instrument
        std::string instrument = names.empty() ? std::string() : names.front();
        if (std::find(names.begin(), names.end(), "Spanish classical guitar") != names.end())
            instrument = "Spanish classical guitar";
        const audio::DeviceSetting device = instrument.empty() ? audio::DeviceSetting{} : audio::SamplerDevice::setting(instrument);
        ui_.post([this, device, options] { replaceProject(daw::newProject(device, *options), {}); });
    }, true);
}

}
