// DawApp's inputs, recording, audio import and editing commands, and the project dialogs
#include "DawApp.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <memory>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "Arrangement.h"
#include "AudioEditor.h"
#include "Format.h"
#include "Mixer.h"
#include "PianoRoll.h"
#include "TransportBar.h"
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
        arrangement_->rebuild();
        mixer_->rebuild();
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
        Clip c(fs::path(file).stem().string(), beat, data->seconds() / project().secondsPerBeat());
        c.audioFile = file;
        auto &clips = session_.store().editTrack(index).clips;
        clips.push_back(c);
        added = static_cast<int>(clips.size()) - 1;
        beat += c.length;
    }
    // The timeline grows to hold them
    const double bar = std::max(1, project().beatsPerBar);
    if (beat > project().lengthBeats())
        session_.store().edit().bars = static_cast<int>(std::ceil(beat / bar)) + 1;
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
    const std::string path = (fs::temp_directory_path() / "Vibin demo take.wav").string();
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
            bridge_->removeTrack(index);
            session_.store().edit().deleteTrack(index);
            arrangement_->rebuild();
            mixer_->rebuild();
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
    const double bar = project().beatsPerBar;
    Dialog &dialog = Dialog::open(gui_, "Clip", "Where it starts on the timeline and how long it is.");
    dialog.setWidth(420.0f);
    Container &body = dialog.body();
    auto &name = labelled(body, "Name").add<TextField>("Clip name");
    name.setText(clip->name).setSize(Size::Small).setGrow(1.0f);
    name.onChange([target](const std::string &text) {
        if (Clip *c = target(); c && !text.empty())
            c->name = text;
    });
    // The start as a bar and a beat in it (from 1), keeping any fraction of a beat
    auto setStart = [this, target](double startBar, double beat) {
        if (Clip *c = target()) {
            const double fraction = c->start - std::floor(c->start);
            const double start = (startBar - 1.0) * project().beatsPerBar + (beat - 1.0) + fraction;
            c->start = std::clamp(start, 0.0, std::max(0.0, project().lengthBeats() - c->length));
        }
    };
    auto &barStepper = labelled(body, "Bar").add<Stepper>(static_cast<float>(std::floor(clip->start / bar)) + 1.0f, 1.0f,
                                                          static_cast<float>(project().bars), 1.0f);
    auto &beatStepper = labelled(body, "Beat").add<Stepper>(static_cast<float>(std::fmod(std::floor(clip->start), bar)) + 1.0f, 1.0f,
                                                            static_cast<float>(bar), 1.0f);
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

void DawApp::openTrackMenu(int track, Vec2 at)
{
    const Track *t = project().track(track);
    if (!t)
        return;
    std::vector<MenuItem> items{
        MenuItem::item("Rename", [this, track] { arrangement_->renameTrack(track); }),
        MenuItem::toggle("Armed for recording", [this, track] {
            const Track *tr = project().track(track);
            return tr && tr->armed;
        }, [this, track] {
            if (project().track(track)) {
                Track &tr = session_.store().editMix(track);
                tr.armed = !tr.armed;
            }
        }),
    };
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
            const std::vector<std::string> names{"10 seconds", "30 seconds", "1 minute", "5 minutes", "Until Vibin closes"};
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

void DawApp::openProjectSettings()
{
    Dialog &dialog = Dialog::open(gui_, "Project settings");
    dialog.setWidth(460.0f);
    Container &body = dialog.body();
    auto &name = labelled(body, "Name").add<TextField>("Project name");
    name.setText(project().name);
    name.setSize(Size::Small).setGrow(1.0f);
    name.onChange([this](const std::string &text) { session_.store().edit().name = text; });
    audio::Transport &transport = audio_.transport();
    labelled(body, "Tempo")
        .add<Stepper>(static_cast<float>(transport.tempo()), 20.0f, 400.0f, 1.0f,
                      [&transport](float bpm) { transport.setTempo(bpm); })
        .setSuffix(" BPM")
        .setSize(Size::Small);
    labelled(body, "Beats per bar")
        .add<Stepper>(static_cast<float>(project().beatsPerBar), 1.0f, 16.0f, 1.0f,
                      [this](float beats) { session_.store().edit().beatsPerBar = static_cast<int>(beats); })
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
