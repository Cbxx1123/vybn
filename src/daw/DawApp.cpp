#include "DawApp.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "Actions.h"
#include "Autosave.h"
#include "Arrangement.h"
#include "AudioEditor.h"
#include "Browser.h"
#include "DevicePicker.h"
#include "DevicesPanel.h"
#include "ExportsPanel.h"
#include "audio/DeviceChain.h"
#include "audio/PluginEffect.h"
#include "audio/PluginInstrument.h"
#include "Format.h"
#include "Mixer.h"
#include "PianoRoll.h"
#include "ProjectFile.h"
#include "ShortcutsEditor.h"
#include "StepMenus.h"
#include "Toml.h"
#include "engine/FileDialog.h"
#include "engine/Shell.h"
#include "TransportBar.h"

namespace daw {

using namespace ui;

namespace {

Container &row(Container &parent)
{
    auto &r = parent.add<Container>(Direction::Row);
    r.setAlign(Align::Center);
    return r;
}

}

DawApp::DawApp(env::Environment &env, UISystem &ui, audio::AudioSystem &audio)
    : env_(env), ui_(ui), audio_(audio), gui_(ui.context()), session_(demoProject()),
      settings_(Settings::load()), library_(std::make_unique<InstrumentLibrary>(env, settings_)),
      online_(std::make_unique<OnlineCatalogue>(env, settings_)),
      downloads_(std::make_unique<Downloads>(env, *library_)), metronome_(audio.add<audio::Metronome>()),
      midi_(audio.add<audio::MidiHub>()), recorder_(audio.add<audio::InputRecorder>()),
      pool_(std::make_unique<AudioPool>(env)),
      preview_(std::make_unique<PreviewPlayer>(audio, *library_, *pool_, ui_.window(),
                                               [this](const std::string &message) { notify(message); })),
      recording_(std::make_unique<RecordingSession>(session_.store(), audio, midi_, recorder_, *pool_))
{
    plugins_ = env.find<plugin::PluginManager>();
    if (plugins_) {
        const std::vector<std::string> base = settings_.pluginConfig().folders;
        for (const std::string &f : plugins_->config().folders)
            if (std::find(base.begin(), base.end(), f) == base.end())
                extraPluginFolders_.push_back(f);
    }
    audio.masterEffects().setPlugins(plugins_);
    catalogue_ = std::make_unique<DeviceCatalogue>(*library_, plugins_);
    bridge_ = std::make_unique<AudioBridge>(audio_, midi_, plugins_, *library_, *pool_);
    recordingController_ = std::make_unique<RecordingController>(
        audio_, metronome_, session_.store(), *recording_, settings_, session_.path(),
        RecordingController::Hooks{[this] { openAudioInput(); }, [this](std::string message) { notify(std::move(message)); }});
    // Exports render from a copy of the project on their own engine, so they start where the project is
    exporter_ = std::make_unique<Exporter>(
        env_, gui_, audio_, plugins_, *library_, *pool_, session_.store(),
        ExporterHooks{[this] { return session_.path().empty() ? std::string() : std::filesystem::path(session_.path()).parent_path().string(); },
                      [this] { showExports(); },
                      [this](std::string message) { notify(std::move(message)); }});
    applyAudioSettings(); // the devices from the settings, before the device starts
    syncInputs();
    audio.transport().setTempo(project().tempo);
    syncTracks(); // the demo project's tracks join the graph before the device starts
    // The keyboard actions and the user's changes to their keys, before the menus show them
    defineActions(ui_.keymap());
    std::string keysError;
    if (!loadKeybinds(ui_.keymap(), keysError)) {
        ui_.post([this, keysError] {
            Dialog::open(gui_, "Some shortcuts weren't loaded", keysError + ". The rest of your shortcuts are in use.")
                .addButton("OK", {}, true);
        });
    }
    // The user's look, from View > Appearance
    std::string themeError;
    Theme saved = gui_.theme();
    if (loadTheme(saved, themePreset_, themeError))
        gui_.theme() = saved;
    else if (!themeError.empty())
        ui_.post([this, themeError] {
            Dialog::open(gui_, "Your theme wasn't loaded", themeError + ". The default look is in use.")
                .addButton("OK", {}, true);
        });
    themeSaved_ = writeTheme(gui_.theme(), themePreset_);
    buildTitleBar();
    buildBody();
    buildMenus();
    buildBrowser();
    session_.history().reset(project());
    // Autosave: every so often while the project has unsaved changes. Settings give the interval;
    // VIBIN_AUTOSAVE_SECONDS (a test hook) overrides it in seconds.
    // Looked for before the controller writes its own session lock over the last session's
    recoverable_ = findRecoverable(autosaveRoot(), unixSeconds());
    autosave_ = std::make_unique<AutosaveController>(&env_, autosaveRoot(), [] { return unixSeconds(); });
    autosave_->configure(settings_.autosave, autosaveInterval(), settings_.autosaveVersions);
}

double DawApp::autosaveInterval() const
{
    double seconds = settings_.autosaveMinutes * 60.0;
    if (const char *override = std::getenv("VIBIN_AUTOSAVE_SECONDS")) {
        if (std::atof(override) > 0.0)
            seconds = std::atof(override);
    }
    return seconds;
}

void DawApp::applyAutosaveSettings()
{
    std::string error;
    settings_.save(error);
    autosave_->configure(settings_.autosave, autosaveInterval(), settings_.autosaveVersions);
}

DawApp::~DawApp()
{
    // A clean exit: waits for an autosave under way, then removes the session lock
    autosave_.reset();
}

void DawApp::buildTitleBar()
{
    // A raised strip lined up with the panels under it: the mark and the menus, the project in the
    // middle, quick buttons and the window's own at the right
    TitleBar &bar = ui_.titleBar();
    bar.setRaised(true, 12.0f);
    bar.add<LogoMark>();
    bar.add<Label>("Vibin", TextStyle::Body, TextTone::Primary);
    menuBar_ = &bar.add<MenuBar>(gui_);
    bar.add<Spacer>();
    bar.add<ProjectBadge>([this] { return project().name; }, [this] { return session_.modified(); });
    bar.add<Spacer>();
    // Quick buttons: pressed in while what they open is showing
    auto quick = [&bar](Icon icon, std::function<void()> onClick, std::function<bool()> on) {
        Button &b = bar.add<Button>(std::move(onClick));
        b.setIcon(icon).setVariant(ButtonVariant::Flat).setShape(Shape::Circle).setSize(Size::Small);
        if (on) {
            Button *raw = &b;
            bar.add<FrameHook>([raw, on](const Input &) { raw->setSelected(on()); });
        }
    };
    quick(Icon::Grid, [this] { toggleBrowser(); }, [this] { return browser_ && browser_->shown(); });
    quick(Icon::Sliders, [this] { showAppearance(); }, [this] { return appearanceOpen(); });
    quick(Icon::Gear, [this] { openSettings(); }, {});
}

void DawApp::buildBody()
{
    Container &body = ui_.body();
    body.setAlign(Align::Stretch);
    body.setPadding(12.0f);

    // First each frame: the output peaks, read once for every meter, and the audio graph
    body.add<FrameHook>([this](const Input &input) {
        session_.store().normalizeIds(); // edits made last frame get their ids before history, autosave and the audio see them
        recordingController_->updateCountIn(); // starts the take when the count-in ends
        // Undo: last frame's edits become a step once nothing is being dragged or typed
        mouseHeld_ = input.down || input.middleDown;
        mouseSinceStep_ = mouseSinceStep_ || mouseHeld_ || input.pressed || input.released;
        typing_ = input.signals && input.signals->focus();
        // Device views: a plugin interface not shown this frame moves its window away, and a view
        // whose place no longer holds its device closes
        for (auto &open : deviceViews_) {
            if (PluginEditorView *editor = open.second.view->editor())
                editor->hideIfUnseen(input.time);
            const audio::DeviceSetting *d = project().device(placeAt(open.second.place, open.second.track));
            if (!open.second.closing && (!d || d->key() != open.second.key)) {
                open.second.closing = true;
                dock_->removePanel(*open.second.panel);
            }
        }
        // A parameter held in a plugin's own window is a drag too
        bool gesturing = false;
        bool pluginBusy = false;
        for (plugin::Slot *slot : bridge_->pluginSlots()) {
            gesturing = gesturing || slot->gesturing();
            pluginBusy = pluginBusy || slot->busy();
        }
        mouseSinceStep_ = mouseSinceStep_ || gesturing;
        // The loop keeps its fastest pace while something changes without input: playback and
        // recording (the playhead, the take), a count-in, a render, a download, a preview, a plugin
        // loading, a sample set loading. Otherwise it sleeps between changes (see UISystem::setBusy).
        // A plugin's interface (its pictures change with the sound) and a live input meter (an armed
        // audio track) need the fast pace too
        bool pluginPictures = false;
        for (const auto &open : deviceViews_)
            pluginPictures = pluginPictures || open.second.view->showingInterface();
        const bool inputLive = std::any_of(project().tracks.begin(), project().tracks.end(),
                                           [](const Track &t) { return t.armed && t.kind == TrackKind::Audio; });
        ui_.setBusy(audio_.transport().playing() || recording_->recording() || recording_->finishing() ||
                    recordingController_->countingIn() || exporter_->queue().active() != nullptr ||
                    downloads_->activeCount() > 0 || preview_->previewing() || library_->anyLoading() || pluginBusy ||
                    pluginPictures || inputLive);
        const bool settled = !mouseHeld_ && !typing_ && !gesturing && !recording_->recording() && !recording_->finishing();
        if (!restoring_ && session_.recordHistory(settled, !mouseSinceStep_, ui_.window().time()))
            mouseSinceStep_ = false;
        // Autosave: a copy of unsaved changes every so often, and the session lock kept on the project's file
        std::string autosaveError;
        switch (autosave_->update(session_, settled, autosaveError)) {
        case AutosaveController::Event::Autosaved:
            notify("Autosaved");
            break;
        case AutosaveController::Event::VersionSaved:
            notify("Version saved");
            break;
        case AutosaveController::Event::Failed:
            notify("Autosave failed: " + autosaveError);
            break;
        case AutosaveController::Event::None:
            break;
        }
        for (uint32_t c = 0; c < 2; ++c)
            levels_.peak[c] = audio_.takePeak(c);
        metronome_.beatsPerBar = project().beatsPerBar;
        session_.store().setTempo(audio_.transport().tempo()); // the tempo is edited on the transport
        // The store's safety net (only with VIBIN_VERIFY_STORE=1): changes its revisions didn't count
        if (env_.frame() % 60 == 0)
            session_.verify();
        // Unsaved changes: the project as it would be saved, against the file, a few times a second
        if (env_.frame() % 15 == 0) {
            session_.checkModified();
            // The look, whenever it changes (the Appearance window, dark mode)
            const std::string look = writeTheme(gui_.theme(), themePreset_);
            std::string error;
            if (look != themeSaved_ && saveTheme(gui_.theme(), themePreset_, error))
                themeSaved_ = look;
            // The piano roll's view (the grid, ghosts, lane...), kept for the next run
            const std::string view = PianoRollView::writeView(pianoRoll_->view());
            if (view != rollViewSaved_) {
                settings_.pianoRollView = view;
                if (settings_.save(error))
                    rollViewSaved_ = view;
            }
            // The step sequencer's view, the same way
            const std::string stepView = StepSequencerView::writeView(steps_->view());
            if (stepView != stepViewSaved_) {
                settings_.stepSequencerView = stepView;
                if (settings_.save(error))
                    stepViewSaved_ = stepView;
            }
        }
        // Inputs: their meters, MIDI arriving, takes being recorded
        for (uint32_t c = 0; c < audio::AudioSystem::kMaxInputs; ++c)
            inputPeaks_[c] = audio_.takeInputPeak(c);
        const std::string message = recording_->update();
        if (!message.empty())
            notify(message);
        if (recording_->midiMessages() != midiSeen_) {
            midiSeen_ = recording_->midiMessages();
            midiTime_ = ui_.window().time();
        }
        syncInputs();
        syncTracks();
        preview_->update();
        exporter_->update();
        // The editor for the selected clip's kind, and its gain control
        const Track *selected = project().track(project().selectedTrack);
        editors_->setCurrent(selected && selected->kind == TrackKind::Audio ? 1 : (stepMode_ ? 2 : 0));
        if (const Clip *c = audioEditor_->clip())
            clipGain_->setValue(std::clamp((20.0f * std::log10(std::max(1e-4f, c->gain)) + 24.0f) / 36.0f, 0.0f, 1.0f));
        clipGain_->setEnabled(audioEditor_->clip() != nullptr);
    });

    RecordHooks record;
    record.recording = [this] { return recording_->recording() || recording_->finishing() || recordingController_->countingIn(); };
    record.toggle = [this] { toggleRecording(); };
    record.countIn = [this] { return recordingController_->countIn(); };
    record.setCountIn = [this](int choice) { recordingController_->setCountIn(choice); };
    record.midiInput = [this] { return midiInputText(); };
    record.midiActive = [this] { return ui_.window().time() - midiTime_ < 0.12; };
    record.audioInput = [this] { return audioInputText(); };
    record.inputLevel = [this] {
        float peak = 0.0f;
        for (float p : inputPeaks_)
            peak = std::max(peak, p);
        return peak;
    };
    record.notice = [this] { return notice(); };
    body.add<TransportBar>(gui_, audio_, metronome_, session_.store(), levels_, record);

    // The arrangement, the mixer, the editor and the effects are dock panels: each can be torn off
    // into a window of its own and docked back anywhere in the grid, which fills the window
    dock_ = &body.add<DockSpace>(ui_, gui_);
    arrangementPanel_ = &dock_->addPanel("Arrangement");
    mixerPanel_ = &dock_->addPanel("Mixer");
    editorPanel_ = &dock_->addPanel("Editor");
    effectsPanel_ = &dock_->addPanel("Devices");
    exportsPanel_ = &dock_->addPanel("Exports");

    // The arrangement: its tools in the panel's header, over the timeline
    audio::AudioSystem &audio = audio_;
    Container &tools = arrangementPanel_->header();
    TransportHooks hooks;
    hooks.position = [&audio] { return transportBeats(audio); };
    hooks.playing = [&audio] { return audio.transport().playing(); };
    hooks.seek = [&audio](double beats) { seekBeats(audio, beats); };
    hooks.openClip = [this] { showEditor(); };
    hooks.audioFor = [this](int track, int clip) { return clipAudio(track, clip); };
    hooks.isRecording = [this](int track, int clip) { return recording_->isRecording(track, clip); };
    hooks.trackInput = [this](int track) { return trackInputText(track); };
    hooks.trackListening = [this](int track) {
        audio::NotePlayer *player = bridge_->player(track);
        return player && player->listen.load();
    };
    hooks.clipMenu = [this](Vec2 at, double beat) { openClipMenu(at, beat); };
    hooks.chooseInstrument = [this](int track, Rect line) {
        session_.store().select(track);
        pickDevice(true, false, track, line);
    };
    hooks.trackMenu = [this](int track, Vec2 at) { openTrackMenu(track, at); };
    arrangement_ = &arrangementPanel_->add<ArrangementView>(session_.store(), hooks);
    arrangement_->setGrow(1.0f);
    ArrangementView &arrangement = *arrangement_;
    tools.add<SmallToggle>("Follow", [&arrangement] { return arrangement.follow(); },
                           [&arrangement](bool on) { arrangement.setFollow(on); });
    tools.add<Button>([&arrangement] { arrangement.zoom(1.0f / 1.25f); })
        .setIcon(Icon::Minus)
        .setShape(Shape::Circle)
        .setSize(Size::Small);
    tools.add<Button>([&arrangement] { arrangement.zoom(1.25f); })
        .setIcon(Icon::Plus)
        .setShape(Shape::Circle)
        .setSize(Size::Small);
    // A menu of the two kinds of track
    Button &addButton = tools.add<Button>();
    Button *addButtonRaw = &addButton;
    addButton.onClick([this, addButtonRaw] {
        const Rect r = addButtonRaw->rect();
        openContextMenu(gui_,
                        {MenuItem::item("Instrument track", [this] { addTrack(TrackKind::Instrument); }),
                         MenuItem::item("Audio track", [this] { addTrack(TrackKind::Audio); })},
                        {r.x, r.y + r.h + 6.0f});
    });
    addButton.setLabel("Add track").setIcon(Icon::Plus).setSize(Size::Small);

    // The mixer, and the editor for the selected clip (its status in the panel's header)
    editorPanel_->header().add<LiveLabel>([this] {
        const Track *t = project().track(project().selectedTrack);
        if (!t || t->kind != TrackKind::Instrument)
            return std::string();
        if (stepMode_)
            return steps_ ? steps_->hint() : std::string();
        return pianoRoll_ ? pianoRoll_->hint() : std::string();
    }, TextStyle::Caption, TextTone::Muted);
    editorPanel_->header().add<LiveLabel>([this] {
        const Track *t = project().track(project().selectedTrack);
        const Clip *c = project().clip();
        if (!t || !c)
            return std::string("No clip selected");
        // The piano roll's last action, selection or velocity under the mouse, else what the
        // mouse can do there
        std::string status;
        if (stepMode_ && steps_) {
            if (t->kind == TrackKind::Instrument)
                status = steps_->message();
        } else if (pianoRoll_ && t->kind == TrackKind::Instrument) {
            status = pianoRoll_->message();
            const int n = pianoRoll_->selectedCount();
            if (status.empty() && n > 0)
                status = format("%.0f selected", static_cast<double>(n));
            const float v = pianoRoll_->shownVelocity();
            if (v >= 0.0f)
                status = format("Velocity %.0f", std::round(v * 127.0f)) + (status.empty() ? "" : "   " + status);
        }
        return (status.empty() ? std::string() : status + "        ") + t->name + "  /  " + c->name;
    }, TextStyle::Caption, TextTone::Secondary);

    mixer_ = &mixerPanel_->add<MixerView>(gui_, session_.store(), audio_, levels_, bridge_->levels(), [this](TrackKind kind) { addTrack(kind); });
    mixer_->setGrow(1.0f);

    editors_ = &editorPanel_->add<Pages>();
    editors_->setGrow(1.0f);
    auto &roll = editors_->add<Container>(Direction::Column);
    roll.setAlign(Align::Stretch);
    PianoRollHooks rollHooks;
    rollHooks.auditionOn = [this](int key, float velocity) {
        if (audio::NotePlayer *player = bridge_->player(project().selectedTrack))
            player->audition(key, velocity);
    };
    // Every player, in case the selection moved while the key was held
    rollHooks.auditionOff = [this](int key) { bridge_->releaseKey(key); };
    rollHooks.position = hooks.position;
    rollHooks.seek = hooks.seek;
    rollHooks.playing = hooks.playing;
    rollHooks.laneMenu = [this](Vec2 at) { openContextMenu(Context::activeOr(gui_), rollMenus_->laneMenu(), at); };
    rollHooks.contextMenu = [this](Vec2 at) {
        std::vector<MenuItem> items = rollMenus_->noteMenu();
        items.push_back(MenuItem::divider());
        items.push_back(MenuItem::sub("Insert", rollMenus_->insertMenu()));
        openContextMenu(Context::activeOr(gui_), items, at);
    };
    // The piano roll's toolbar, one compact row: the tool; the grid and the scale as menus that
    // name what is set; the selected notes' key, start, length and velocity to drag or wheel,
    // and mute; then the loop and the View and Notes menus. (The piano roll is made after its
    // toolbar, so the callbacks check it exists.)
    auto &rollTools = row(roll);
    // Piano roll or Steps, the editor's two pages for an instrument clip: a segmented control kept in
    // step with the mode (the steps' toolbar has one too)
    auto modeControl = [this](Container &bar, int selected) {
        SegmentedControl &control = bar.add<SegmentedControl>(std::vector<std::string>{"Piano roll", "Steps"}, selected,
                                                              [this](int i) { setStepMode(i == 1); });
        control.setSize(Size::Small);
        SegmentedControl *raw = &control;
        bar.add<FrameHook>([this, raw](const Input &) {
            const int want = stepMode_ ? 1 : 0;
            if (raw->selected() != want)
                raw->setSelected(want);
        });
    };
    modeControl(rollTools, 0);
    auto &tool = rollTools.add<SegmentedControl>(std::vector<std::string>{"Draw", "Select", "Slice", "Erase"}, 0, [this](int i) {
        const PianoRollView::Tool tools[] = {PianoRollView::Tool::Draw, PianoRollView::Tool::Select, PianoRollView::Tool::Slice,
                                             PianoRollView::Tool::Erase};
        pianoRoll_->setTool(tools[std::clamp(i, 0, 3)]);
    });
    tool.setSize(Size::Small);
    SegmentedControl *toolRaw = &tool;
    rollTools.add<FrameHook>([this, toolRaw](const Input &) { toolRaw->setSelected(static_cast<int>(pianoRoll_->tool())); });
    // A button opening a menu, its label following what the menu sets
    auto menuButton = [this](Container &bar, std::function<std::string()> label, std::function<std::vector<MenuItem>()> items,
                             float width) {
        Button &b = bar.add<Button>();
        Button *raw = &b;
        b.onClick([this, raw, items] {
            const Rect r = raw->rect();
            openContextMenu(Context::activeOr(gui_), items(), {r.x, r.y + r.h + 6.0f});
        });
        b.setLabel(label()).setSize(Size::Small).setMinSize({width, 0.0f});
        auto shown = std::make_shared<std::string>(label());
        bar.add<FrameHook>([raw, label, shown](const Input &) {
            std::string now = label();
            if (now != *shown) {
                *shown = now;
                raw->setLabel(now);
            }
        });
    };
    menuButton(rollTools, [this] {
        if (!pianoRoll_)
            return std::string("Grid");
        return (pianoRoll_->snapping() ? std::string() : std::string("Free ")) + pianoRoll_->gridText() +
               (pianoRoll_->grid() == 0 ? " auto" : "");
    }, [this] { return rollMenus_->gridMenu(); }, 92.0f);
    menuButton(rollTools, [this] {
        const Scale *s = pianoRoll_ ? pianoRoll_->scale() : nullptr;
        return s ? std::string(rootName(pianoRoll_->root())) + " " + s->name : std::string("No scale");
    }, [this] { return rollMenus_->scaleMenu(); }, 132.0f);

    auto selected = [this] { return pianoRoll_ && pianoRoll_->hasSelectedNote(); };
    auto first = [this] { return pianoRoll_ ? pianoRoll_->firstSelected() : nullptr; };
    rollTools.add<ScrubField>("Key", [first] {
        const Note *n = first();
        return n ? audio::keyName(n->key) : std::string();
    }, [this](int steps, bool fine) { pianoRoll_->transposeSelection(steps * (fine ? 12 : 1)); }, selected).setValueWidth(34.0f);
    rollTools.add<ScrubField>("Start", [this, first] {
        const Note *n = first();
        const Clip *c = pianoRoll_->clip();
        return n && c ? barsBeats(c->start + n->start, std::max(1, project().beatsPerBar)) : std::string();
    }, [this](int steps, bool fine) { pianoRoll_->nudgeSelection(steps * pianoRoll_->snap() / (fine ? 4.0 : 1.0)); }, selected)
        .setValueWidth(62.0f);
    rollTools.add<ScrubField>("Length", [this, first] {
        const Note *n = first();
        if (!n)
            return std::string();
        const std::string name = gridName(n->length, std::max(1, project().beatsPerBar));
        return name.find("beats") == std::string::npos ? name : format("%.3g", n->length);
    }, [this](int steps, bool fine) { pianoRoll_->resizeSelection(steps * pianoRoll_->snap() / (fine ? 4.0 : 1.0)); }, selected)
        .setValueWidth(44.0f);
    rollTools.add<ScrubField>("Vel", [first] {
        const Note *n = first();
        return n ? format("%.0f", std::round(n->velocity * 127.0f)) : std::string();
    }, [this](int steps, bool fine) { pianoRoll_->velocitySelection(static_cast<float>(steps) * (fine ? 1.0f : 4.0f) / 127.0f); },
        selected).setValueWidth(26.0f);
    rollTools.add<SmallToggle>("Mute", [first] {
        const Note *n = first();
        return n && n->muted;
    }, [this](bool) { pianoRoll_->toggleMute(); });
    rollTools.add<Spacer>();
    rollTools.add<SmallToggle>("Loop", [this] { return project().looping; }, [this](bool on) {
        if (on && !project().hasLoop())
            pianoRoll_->loopSelection();
        else
            session_.store().setLoop(project().loopStart, project().loopEnd, on && project().hasLoop());
    });
    menuButton(rollTools, [] { return std::string("View"); }, [this] { return rollMenus_->rollViewMenu(); }, 0.0f);
    menuButton(rollTools, [] { return std::string("Notes"); }, [this] {
        std::vector<MenuItem> items = rollMenus_->noteMenu();
        items.push_back(MenuItem::divider());
        items.push_back(MenuItem::sub("Insert", rollMenus_->insertMenu()));
        return items;
    }, 0.0f);

    pianoRoll_ = &roll.add<PianoRollView>(session_.store(), rollHooks);
    pianoRoll_->setGrow(1.0f);
    rollMenus_ = std::make_unique<PianoRollMenus>(env_, gui_, *pianoRoll_);
    pianoRoll_->setView(PianoRollView::readView(settings_.pianoRollView));
    rollViewSaved_ = PianoRollView::writeView(pianoRoll_->view());

    // The audio editor: gain and fades for the selected audio clip
    auto &wave = editors_->add<Container>(Direction::Column);
    wave.setAlign(Align::Stretch);
    auto &waveTools = row(wave);
    waveTools.add<Label>("Gain", TextStyle::Caption, TextTone::Secondary);
    clipGain_ = &waveTools.add<Slider>(24.0f / 36.0f, [this](float p) {
        if (audioEditor_->clip())
            session_.store().editClip(project().selectedTrack, project().selectedClip).gain = std::pow(10.0f, (p * 36.0f - 24.0f) / 20.0f);
    });
    clipGain_->setSize(Size::Small).setMinSize({140.0f, 0.0f});
    waveTools.add<LiveLabel>([this] {
        const Clip *c = audioEditor_ ? audioEditor_->clip() : nullptr;
        if (!c)
            return std::string();
        return decibels(c->gain) + format(",  fade in %.2f s", c->fadeIn) + format(", out %.2f s", c->fadeOut);
    }, TextStyle::Caption, TextTone::Secondary);
    waveTools.add<Spacer>();
    waveTools.add<Label>("Drag the edges to trim, the top corners to fade; double-click to split", TextStyle::Caption,
                         TextTone::Muted);
    waveTools.add<Button>([this] { splitSelectedClip(); }).setLabel("Split at playhead").setSize(Size::Small);
    AudioEditorHooks waveHooks;
    waveHooks.audioFor = [this](const Clip &c) { return c.hasAudio() ? pool_->get(c.audioFile).get() : nullptr; };
    waveHooks.position = hooks.position;
    waveHooks.seek = hooks.seek;
    audioEditor_ = &wave.add<AudioEditorView>(session_.store(), waveHooks);
    audioEditor_->setGrow(1.0f);
    AudioEditorView &audioEditor = *audioEditor_;
    waveTools.add<Button>([&audioEditor] { audioEditor.fit(); }).setLabel("Fit").setVariant(ButtonVariant::Flat).setSize(Size::Small);
    waveTools.add<Button>([&audioEditor] { audioEditor.zoom(1.0f / 1.25f); })
        .setIcon(Icon::Minus)
        .setShape(Shape::Circle)
        .setSize(Size::Small);
    waveTools.add<Button>([&audioEditor] { audioEditor.zoom(1.25f); })
        .setIcon(Icon::Plus)
        .setShape(Shape::Circle)
        .setSize(Size::Small);

    // The step sequencer, the editor's third page: a drum machine's grid over the clip's notes, with its
    // toolbar (the step size, swing, and the gate and velocity of new steps) and the Loop and Add row tools
    auto &stepPage = editors_->add<Container>(Direction::Column);
    stepPage.setAlign(Align::Stretch);
    auto &stepTools = row(stepPage);
    modeControl(stepTools, 1);
    menuButton(stepTools, [this] {
        if (!steps_)
            return std::string("Step");
        return std::string("Step ") + stepChoices()[static_cast<size_t>(steps_->stepChoice())].name;
    }, [this] { return stepSizeMenu(*steps_); }, 86.0f);
    stepTools.add<Label>("Swing", TextStyle::Caption, TextTone::Secondary);
    Slider &swing = stepTools.add<Slider>(0.0f, [this](float v) { steps_->setSwing(v); });
    swing.setSize(Size::Small).setMinSize({96.0f, 0.0f});
    Slider *swingRaw = &swing;
    stepTools.add<FrameHook>([this, swingRaw](const Input &) {
        const float v = static_cast<float>(steps_->swing());
        if (swingRaw->value() != v)
            swingRaw->setValue(v);
    });
    stepTools.add<ScrubField>("Gate", [this] { return format("%.0f%%", steps_->gate() * 100.0); },
                              [this](int count, bool fine) { steps_->setGate(steps_->gate() + count * (fine ? 0.01 : 0.05)); })
        .setValueWidth(40.0f);
    stepTools.add<ScrubField>("Vel", [this] { return format("%.0f", std::round(steps_->velocity() * 127.0f)); },
                              [this](int count, bool fine) {
                                  steps_->setVelocity(steps_->velocity() + static_cast<float>(count) * (fine ? 1.0f : 4.0f) / 127.0f);
                              })
        .setValueWidth(26.0f);
    stepTools.add<SmallToggle>("Hear", [this] { return steps_->audition(); }, [this](bool on) { steps_->setAudition(on); });
    stepTools.add<Spacer>();
    stepTools.add<SmallToggle>("Loop", [this] { return project().looping; }, [this](bool on) {
        if (on && !project().hasLoop())
            pianoRoll_->loopSelection();
        else
            session_.store().setLoop(project().loopStart, project().loopEnd, on && project().hasLoop());
    });
    menuButton(stepTools, [] { return std::string("Add row"); }, [this] { return stepAddRowMenu(*steps_); }, 0.0f);

    // The view is made after its toolbar, so the callbacks check it exists
    StepSequencerHooks stepHooks;
    stepHooks.auditionOn = rollHooks.auditionOn;
    stepHooks.auditionOff = rollHooks.auditionOff;
    stepHooks.position = hooks.position;
    stepHooks.playing = hooks.playing;
    stepHooks.rowMenu = [this](int key, Vec2 at) {
        openContextMenu(Context::activeOr(gui_), stepRowMenu(Context::activeOr(gui_), *steps_, key), at);
    };
    stepHooks.addRowMenu = [this](Vec2 at) { openContextMenu(Context::activeOr(gui_), stepAddRowMenu(*steps_), at); };
    steps_ = &stepPage.add<StepSequencerView>(session_.store(), stepHooks);
    steps_->setGrow(1.0f);
    steps_->setView(StepSequencerView::readView(settings_.stepSequencerView));
    stepViewSaved_ = StepSequencerView::writeView(steps_->view());

    // The effects: the chain of the selected track, or of the master bus
    {
        auto &target = effectsPanel_->header().add<SegmentedControl>(std::vector<std::string>{"Track", "Master"}, 0, [this](int i) {
            effects_->setMaster(i == 1);
        });
        target.setSize(Size::Small);
        effectsPanel_->header().add<LiveLabel>([this] { return effects_ ? effects_->targetName() : std::string(); }, TextStyle::Caption,
                                               TextTone::Secondary);
        DevicesHooks devicesHooks;
        devicesHooks.device = [this](const DevicePlace &place) { return bridge_->deviceAt(project(), place); };
        devicesHooks.openDevice = [this](const DevicePlace &place) { openDevice(place); };
        devicesHooks.pick = [this](bool instrument, bool master, int track, Rect anchor) { pickDevice(instrument, master, track, anchor); };
        devicesHooks.latencyText = [this](int track) { return bridge_->latencyText(track); };
        devicesHooks.instrumentStatus = [this](int track) { return bridge_->instrumentStatus(project(), track); };
        effects_ = &effectsPanel_->add<DevicesView>(gui_, session_.store(), *catalogue_, devicesHooks);
        effects_->setGrow(1.0f);
    }

    // The exports: the render queue, its progress and what each render wrote
    {
        ExportsHooks exportsHooks;
        exportsHooks.openDialog = [this] { openExportDialog(); };
        ExportsView &exports = exportsPanel_->add<ExportsView>(gui_, session_.store(), exporter_->queue(), exportsHooks);
        exports.setGrow(1.0f);
    }

    // The arrangement over the mixer, with the editor, the devices and the exports as the mixer's other tabs
    dock_->dock(*arrangementPanel_, nullptr, DockSide::Center);
    dock_->dock(*mixerPanel_, arrangementPanel_, DockSide::Bottom, 0.55f);
    dock_->dock(*editorPanel_, mixerPanel_, DockSide::Center);
    dock_->dock(*effectsPanel_, mixerPanel_, DockSide::Center);
    dock_->dock(*exportsPanel_, mixerPanel_, DockSide::Center);
    dock_->show(*mixerPanel_);

}

void DawApp::syncTracks()
{
    // The piano roll's loop, except while a render or a take needs the timeline straight
    setLoopBeats(audio_, project().loopStart, project().loopEnd, project().looping && !recording_->recording());
    const AudioBridge::SyncOptions options;
    bridge_->sync(session_.store(), options);
}

void DawApp::showEditor()
{
    // The editors want height (the piano roll for its keys); the mixer has its own split
    if (!dock_->isShowing(*editorPanel_))
        dock_->setShare(*editorPanel_, 0.64f);
    dock_->show(*editorPanel_);
    pianoRoll_->reveal();
    audioEditor_->fit();
}

void DawApp::keepRollView()
{
    rollViewSaved_ = PianoRollView::writeView(pianoRoll_->view());
}

void DawApp::setEditorShare(float share)
{
    dock_->setShare(*editorPanel_, share);
}

void DawApp::setStepMode(bool on)
{
    // The frame hook shows the page for the selected clip's kind
    stepMode_ = on;
}

void DawApp::stepsDemo()
{
    // A Drums track with a two-bar beat on 1/16 steps: kick, snare, closed hats, and an open hat on step 30
    const int index = session_.store().edit().addTrack(TrackKind::Instrument);
    std::vector<Note> notes;
    auto hit = [&notes](int key, int step, float velocity) {
        Note n;
        n.start = step * 0.25;
        n.length = 0.125;
        n.key = key;
        n.velocity = velocity;
        notes.push_back(n);
    };
    for (int s = 0; s < 32; s += 4)
        hit(36, s, 0.85f);
    for (int s = 4; s < 32; s += 8)
        hit(38, s, 0.85f);
    for (int s = 0; s <= 30; s += 2)
        hit(42, s, 0.6f);
    hit(46, 30, 0.85f);
    std::stable_sort(notes.begin(), notes.end(), [](const Note &a, const Note &b) { return a.start < b.start; });
    // The track is made here, not through addTrack: that posts the change and then asks for an instrument
    Track &track = session_.store().editTrack(index);
    track.name = "Drums";
    track.clips.push_back(Clip("Beat", 0.0, 2.0 * std::max(1, project().beatsPerBar), notes));
    session_.store().select(index, static_cast<int>(track.clips.size()) - 1);
    steps_->selectRow(36);
    setStepMode(true);
    showEditor();
    setEditorShare(0.8f);
    // The step view as it is counts as saved (a demo's changes aren't the user's)
    stepViewSaved_ = StepSequencerView::writeView(steps_->view());
}

void DawApp::showEffects()
{
    if (!dock_->isShowing(*effectsPanel_))
        dock_->setShare(*effectsPanel_, 0.5f);
    dock_->show(*effectsPanel_);
}

void DawApp::showExports()
{
    if (!dock_->isShowing(*exportsPanel_))
        dock_->setShare(*exportsPanel_, 0.72f);
    dock_->show(*exportsPanel_);
}

void DawApp::showMixer()
{
    if (!dock_->isShowing(*mixerPanel_))
        dock_->setShare(*mixerPanel_, 0.55f);
    dock_->show(*mixerPanel_);
}

void DawApp::importInstrument()
{
    ui::FileDialogOptions options;
    options.title = "Import instrument";
    options.filters = {{"Instruments (SFZ, or audio files named by note)", "*.sfz;*.wav;*.flac;*.mp3"}, {"All files", "*.*"}};
    options.multiple = true;
    options.action = "Import";
    options.remember = "instrument";
    options.folder = engine::knownFolder(engine::KnownFolder::Downloads);
    chooseFiles(std::move(options), [this](std::vector<std::string> files) {
        std::string error;
        const std::string name = library_->import(files, error);
        if (name.empty()) {
            Dialog::open(gui_, "Import failed", error).addButton("OK", {}, true);
            return;
        }
        // Tracks that asked for it before it was installed load it now
        bridge_->reloadInstrument(name);
        // The selected instrument track gets it if it has none
        const Track *t = project().track(project().selectedTrack);
        if (t && t->kind == TrackKind::Instrument && t->instrument.empty())
            session_.store().editTrack(project().selectedTrack).instrument = audio::SamplerDevice::setting(name);
        Dialog::open(gui_, "Instrument imported",
                     "\"" + name + "\" is now in your library, in " + library_->instrumentsFolder() +
                         ". Choose it for a track from the track's instrument line, or in the browser (Tab).")
            .addButton("OK", {}, true);
    });
}

void DawApp::chooseFiles(ui::FileDialogOptions options, ui::FileDialog::Done done)
{
    if (settings_.nativeFileDialogs) {
        // Windows' dialogs block until they close, so after this frame
        ui_.post([options = std::move(options), done = std::move(done)] {
            std::vector<std::string> paths;
            if (options.save) {
                const std::string path = engine::saveFileDialog(options.title, options.filters, options.folder,
                                                                options.name, options.extension);
                if (!path.empty())
                    paths.push_back(path);
            } else {
                paths = engine::openFileDialog(options.title, options.filters, options.multiple, options.folder);
            }
            if (!paths.empty())
                done(std::move(paths));
        });
        return;
    }
    if (!session_.path().empty())
        options.places.push_back({"This project", std::filesystem::path(session_.path()).parent_path().string(), Icon::Folder});
    options.places.push_back({"Projects", (std::filesystem::path(settings_.libraryPath) / "Projects").string(), Icon::Folder});
    options.places.push_back({"Sample library", settings_.libraryPath, Icon::Keys});
    options.environment = &env_;
    ui::FileDialog::open(gui_, std::move(options), std::move(done));
}

std::string DawApp::projectsFolder() const
{
    if (!session_.path().empty())
        return std::filesystem::path(session_.path()).parent_path().string();
    std::error_code ec;
    const std::filesystem::path folder = std::filesystem::path(settings_.libraryPath) / "Projects";
    std::filesystem::create_directories(folder, ec);
    return folder.string();
}

void DawApp::replaceProject(Project project, std::string path, const Project *saved)
{
    recording_->stop();
    audio::Transport &transport = audio_.transport();
    transport.stop();
    transport.locate(0);
    transport.setTempo(project.tempo);
    project.tempo = transport.tempo(); // as the transport clamped it

    bridge_->dropAll();
    // Sound files only the old project used can go
    std::set<std::string> files;
    for (const Track &t : project.tracks) {
        for (const Clip &c : t.clips) {
            if (c.hasAudio())
                files.insert(c.audioFile);
        }
    }
    pool_->keepOnly(files);
    if (project.selectedTrack < 0 && !project.tracks.empty())
        project.select(0);
    session_.replace(std::move(project), std::move(path), saved); // the model, its file, saved, and a new history
    syncTracks();
    pianoRoll_->reveal();
}

void DawApp::undo()
{
    if (recording_->recording() || recording_->finishing()) {
        notify("Stop recording before undoing");
        return;
    }
    if (mouseHeld_ || typing_ || restoring_)
        return;
    const Project *state = session_.history().undo(project(), ui_.window().time());
    if (!state) {
        notify("Nothing to undo");
        return;
    }
    notify("Undo " + session_.history().redoLabel());
    restoring_ = true;
    ui_.post([this, copy = *state]() mutable { restore(std::move(copy)); });
}

void DawApp::redo()
{
    if (recording_->recording() || recording_->finishing() || mouseHeld_ || typing_ || restoring_)
        return;
    const std::string label = session_.history().redoLabel();
    const Project *state = session_.history().redo(project());
    if (!state) {
        notify("Nothing to redo");
        return;
    }
    notify("Redo " + label);
    restoring_ = true;
    ui_.post([this, copy = *state]() mutable { restore(std::move(copy)); });
}

void DawApp::restore(Project state)
{
    restoring_ = false;
    // Arming and monitoring aren't part of the history: they stay as they are now, kept by track id
    for (Track &t : state.tracks) {
        const Track *now = project().findTrack(t.id);
        if (now && now->kind == t.kind) {
            t.armed = now->armed;
            t.monitor = now->monitor;
        }
    }
    // Nor is the loop
    state.loopStart = project().loopStart;
    state.loopEnd = project().loopEnd;
    state.looping = project().looping;
    audio_.transport().setTempo(state.tempo);
    // The store's replace: the history and the saved text stay as they are. The bridge matches the
    // tracks by id in sync, so a kept track keeps its processor and plugins.
    session_.store().replace(std::move(state));
    syncTracks();
}

void DawApp::confirmDiscard(std::function<void()> then)
{
    session_.checkModified();
    if (!session_.modified()) {
        then();
        return;
    }
    Dialog &dialog = Dialog::open(gui_, "Save changes?",
                                  "\"" + project().name + "\" has changes that aren't saved. They'll be lost if you don't save them.");
    dialog.setWidth(460.0f);
    dialog.addButton("Don't save", [then] { then(); });
    dialog.addButton("Cancel");
    dialog.addButton("Save", [this, then] { ui_.post([this, then] { save(false, then); }); }, true);
}

void DawApp::newProject()
{
    ui_.post([this] {
        // After the unsaved-changes question (if any), the new project's settings
        confirmDiscard([this] { ui_.post([this] { openNewProjectDialog(); }); });
    });
}

void DawApp::openProject()
{
    ui_.post([this] {
        confirmDiscard([this] {
            ui::FileDialogOptions options;
            options.title = "Open project";
            options.filters = {{"Vibin projects (*.vbp)", "*.vbp"}, {"All files", "*.*"}};
            options.folder = projectsFolder();
            options.remember = "project";
            chooseFiles(std::move(options), [this](std::vector<std::string> files) { openProjectFile(files.front()); });
        });
    });
}

bool DawApp::openProjectFile(const std::string &path)
{
    Project loaded;
    std::string error;
    if (!loadProjectFile(path, loaded, error)) {
        Dialog::open(gui_, "Couldn't open the project", error).setWidth(480.0f).addButton("OK", {}, true);
        return false;
    }
    replaceProject(std::move(loaded), path);
    reportMissingInstruments();
    return true;
}

void DawApp::reportMissingInstruments()
{
    const auto &installed = library_->instruments();
    std::vector<std::string> missing;
    for (const std::string &name : instrumentsUsed(project())) {
        if (std::find(installed.begin(), installed.end(), name) == installed.end())
            missing.push_back("\"" + name + "\"");
    }
    if (missing.empty())
        return;
    std::string list;
    for (size_t i = 0; i < missing.size(); ++i)
        list += (i == 0 ? "" : i + 1 == missing.size() ? " and " : ", ") + missing[i];
    Dialog::open(gui_, "Missing instruments",
                 "This project plays " + list + (missing.size() == 1 ? ", which isn't" : ", which aren't") +
                     " in your sample library (" + library_->instrumentsFolder() +
                     "). Those tracks stay silent until you download " + (missing.size() == 1 ? "it" : "them") +
                     " in the browser's Online tab (Tab) or import " + (missing.size() == 1 ? "it" : "them") +
                     " with File > Import instrument.")
        .setWidth(500.0f)
        .addButton("OK", {}, true);
}

void DawApp::saveProject()
{
    ui_.post([this] { save(false); });
}

void DawApp::saveProjectAs()
{
    ui_.post([this] { save(true); });
}

void DawApp::save(bool ask, std::function<void()> after)
{
    if (!ask && !session_.path().empty()) {
        saveTo(session_.path(), after);
        return;
    }
    ui::FileDialogOptions options;
    options.title = "Save project";
    options.save = true;
    options.filters = {{"Vibin projects (*.vbp)", "*.vbp"}};
    options.folder = projectsFolder();
    options.name = project().name;
    options.extension = kProjectExtension;
    options.remember = "project";
    chooseFiles(std::move(options), [this, after](std::vector<std::string> paths) {
        const std::string &path = paths.front();
        // A new project takes its name from its file
        if (project().name.empty() || project().name == "Untitled")
            session_.store().edit().name = std::filesystem::path(path).stem().string();
        saveTo(path, after);
    });
}

void DawApp::setInstrument(int track, const audio::DeviceSetting &device)
{
    if (device.empty())
        return;
    const Track *t = project().track(track);
    if (t && t->kind == TrackKind::Instrument) {
        // The same device again keeps its values
        if (DeviceCatalogue::keyOf(t->instrument) != DeviceCatalogue::keyOf(device))
            session_.store().editTrack(track).instrument = device;
        session_.store().select(track);
        return;
    }
    // A new instrument track for it, named after it (changes the tree: after this frame)
    ui_.post([this, device] {
        Project &freshProject = session_.store().edit();
        const int index = freshProject.addTrack(TrackKind::Instrument);
        Track &fresh = freshProject.tracks[static_cast<size_t>(index)];
        fresh.instrument = device;
        fresh.name = device.name;
        freshProject.select(index);
    });
}

void DawApp::useDevice(const std::string &key)
{
    if (const CatalogueEntry *e = catalogue_->find(key); e && e->instrument)
        setInstrument(project().selectedTrack, e->device);
}

void DawApp::pickDevice(bool instrument, bool master, int track, Rect anchor)
{
    const Track *t = project().track(track);
    DevicePickerOptions options;
    options.instruments = instrument;
    if (instrument) {
        options.title = t && t->kind == TrackKind::Instrument ? "Instrument for " + t->name : "Instrument for a new track";
        options.current = t && t->kind == TrackKind::Instrument ? DeviceCatalogue::keyOf(t->instrument) : std::string();
        options.onPick = [this, track](const CatalogueEntry &e) {
            setInstrument(track, e.device);
            showEffects();
        };
        options.browse = [this] { browser_->chooseInstrument(project().track(project().selectedTrack) ?
                                                                 DeviceCatalogue::keyOf(project().track(project().selectedTrack)->instrument)
                                                                                                    : std::string()); };
    } else {
        options.title = master ? "Add an effect to the master" : t ? "Add an effect to " + t->name : "Add an effect";
        options.onPick = [this, master, track](const CatalogueEntry &e) {
            const std::vector<audio::DeviceSetting> *current = project().effects(master, track);
            if (!current || current->size() >= audio::kMaxEffects)
                return;
            std::vector<audio::DeviceSetting> &chain = master ? session_.store().editMasterEffects()
                                                              : session_.store().editTrack(track).effects;
            chain.push_back(e.device);
        };
    }
    options.managePlugins = [this] { openSettings("Plugins"); };
    if (anchor.w <= 0.0f) {
        const Rect b = gui_.bounds();
        anchor = {b.x + b.w * 0.5f - 310.0f, b.y + 90.0f, 0.0f, 0.0f};
    }
    options.anchor = anchor;
    DevicePicker::open(Context::activeOr(gui_), *catalogue_, std::move(options));
}

void DawApp::saveTo(const std::string &path, std::function<void()> after)
{
    session_.store().normalizeIds(); // a save can run before this frame's hook: the file gets usable ids
    bridge_->captureStates(session_.store());
    std::string error;
    if (!saveProjectFile(path, project(), error)) {
        Dialog::open(gui_, "Couldn't save the project", error).setWidth(480.0f).addButton("OK", {}, true);
        return;
    }
    session_.markSaved(path);
    if (after)
        after();
}

void DawApp::quit()
{
    ui_.post([this] { confirmDiscard([this] { ui_.window().requestClose(); }); });
}

void DawApp::addTrack(TrackKind kind)
{
    // Changes the tree, so after this frame's input
    ui_.post([this, kind] {
        const int index = session_.store().edit().addTrack(kind);
        if (kind == TrackKind::Instrument)
            pickDevice(true, false, index); // what it plays
    });
}

void DawApp::setDarkMode(bool dark)
{
    gui_.theme().setDarkMode(dark);
}

void DawApp::buildMenus()
{
    audio::Transport &transport = audio_.transport();
    ArrangementView &arrangement = *arrangement_;
    audio::Metronome &metronome = metronome_;
    Context &gui = gui_;
    auto hasClip = [this] { return project().clip() != nullptr; };

    // Five menus: the project and its files, editing, the transport, the view, help
    menuBar_->addMenu("Project", {
        MenuItem::item("New project", [this] { newProject(); }, "project.new"),
        MenuItem::item("Open project...", [this] { openProject(); }, "project.open"),
        MenuItem::item("Save", [this] { saveProject(); }, "project.save"),
        MenuItem::item("Save as...", [this] { saveProjectAs(); }, "project.saveAs"),
        MenuItem::item("Save version...", [this] { openSaveVersion(); }, "project.saveVersion"),
        MenuItem::item("Versions...", [this] { openVersions(); }, "project.versions"),
        MenuItem::item("Export...", [this] { openExportDialog(); }, "project.export"),
        MenuItem::divider(),
        MenuItem::item("Import audio file...", [this] { importAudio(); }, "file.importAudio"),
        MenuItem::item("Import instrument...", [this] { importInstrument(); }, "file.importInstrument"),
        MenuItem::item("Import plugin...", [this] { importPlugin(); }),
        MenuItem::divider(),
        MenuItem::item("Project settings...", [this] { openProjectSettings(); }, "project.settings"),
        MenuItem::item("Settings...", [this] { openSettings(); }, "app.settings"),
        MenuItem::divider(),
        MenuItem::item("Quit", [this] { quit(); }, "app.quit"),
    });
    menuBar_->addMenu("Edit", {
        MenuItem::item("Undo", [this] { undo(); }, "edit.undo")
            .labelled([this] { return session_.history().canUndo() ? "Undo " + session_.history().undoLabel() : std::string("Undo"); })
            .when([this] { return session_.history().canUndo() || !sameContent(project(), session_.history().current()); }),
        MenuItem::item("Redo", [this] { redo(); }, "edit.redo")
            .labelled([this] { return session_.history().canRedo() ? "Redo " + session_.history().redoLabel() : std::string("Redo"); })
            .when([this] { return session_.history().canRedo(); }),
        MenuItem::divider(),
        MenuItem::item("Add audio track", [this] { addTrack(TrackKind::Audio); }, "track.addAudio"),
        MenuItem::item("Add instrument track", [this] { addTrack(TrackKind::Instrument); }, "track.addInstrument"),
        MenuItem::item("Delete track", [this] { deleteTrack(project().selectedTrack); }, "track.delete")
            .when([this] { return project().track(project().selectedTrack) != nullptr; }),
        MenuItem::divider(),
        MenuItem::item("Split clip at playhead", [this] { splitSelectedClip(); }, "clip.split").when(hasClip),
        MenuItem::item("Duplicate clip", [this] { session_.store().edit().duplicateClip(); }, "clip.duplicate").when(hasClip),
        MenuItem::item("Delete clip", [this] { session_.store().edit().deleteClip(); }, "edit.delete").when(hasClip),
    });
    menuBar_->addMenu("Transport", {
        MenuItem::item("Play / pause", [&transport] { transport.toggle(); }, "transport.playPause"),
        MenuItem::item("Stop", [&transport] { transport.stop(); }, "transport.stop"),
        MenuItem::item("Return to start", [&transport] { transport.locate(0); }, "transport.toStart"),
        MenuItem::toggle("Record", [this] { return recording_->recording(); }, [this] { toggleRecording(); },
                         "transport.record"),
        MenuItem::divider(),
        MenuItem::toggle("Metronome", [&metronome] { return metronome.enabled.load(); },
                         [&metronome] { metronome.enabled = !metronome.enabled; }, "transport.metronome"),
        MenuItem::toggle("Loop", [this] { return project().looping; }, [this] { session_.store().toggleLoop(); }, "transport.loop"),
        MenuItem::toggle("Follow playhead", [&arrangement] { return arrangement.follow(); },
                         [&arrangement] { arrangement.setFollow(!arrangement.follow()); }, "view.follow"),
    });
    menuBar_->addMenu("View", {
        MenuItem::toggle("Browser", [this] { return browser_ && browser_->shown(); }, [this] { toggleBrowser(); },
                         "view.browser"),
        MenuItem::toggle("Mixer", [this] { return dock_->isShowing(*mixerPanel_); }, [this] { showMixer(); }, "view.mixer"),
        MenuItem::toggle("Editor", [this] { return dock_->isShowing(*editorPanel_); }, [this] { showEditor(); }, "view.editor"),
        MenuItem::toggle("Step sequencer", [this] { return stepMode_; }, [this] {
            setStepMode(!stepMode_);
            showEditor();
        }, "view.steps"),
        MenuItem::toggle("Devices", [this] { return dock_->isShowing(*effectsPanel_); }, [this] { showEffects(); }, "view.effects"),
        MenuItem::toggle("Exports", [this] { return dock_->isShowing(*exportsPanel_); }, [this] { showExports(); }, "view.exports"),
        MenuItem::divider(),
        MenuItem::item("Zoom in", [&arrangement] { arrangement.zoom(1.25f); }, "view.zoomIn"),
        MenuItem::item("Zoom out", [&arrangement] { arrangement.zoom(1.0f / 1.25f); }, "view.zoomOut"),
        MenuItem::divider(),
        MenuItem::toggle("Appearance", [this] { return appearanceOpen(); }, [this] { showAppearance(); },
                         "view.appearance"),
        MenuItem::toggle("Dark mode", [&gui] { return gui.theme().isDark(); },
                         [&gui] { gui.theme().setDarkMode(!gui.theme().isDark()); }, "view.darkMode"),
    });
    menuBar_->addMenu("Help", {
        MenuItem::item("Keyboard shortcuts", [this] { showShortcuts(); }, "help.shortcuts"),
        MenuItem::item("About", [this] { showAbout(); }),
    });
}

void DawApp::openSettings(const std::string &section)
{
    audio::AudioSystem &system = audio_;
    Dialog &dialog = Dialog::open(gui_, "Settings");
    // Wider than the widest section (Recording), so every section gets the same width
    dialog.setWidth(860.0f);

    // Sections in a sidebar on the left; one page at a time on the right, each scrolling
    const float pageHeight = std::min(600.0f, std::max(320.0f, gui_.bounds().h - 240.0f));
    auto &split = dialog.body().add<Container>(Direction::Row);
    split.setAlign(Align::Stretch);
    auto &nav = split.add<ListView>();
    nav.setAlign(Align::Stretch);
    nav.setMinSize({180.0f, 0.0f});
    auto &pages = split.add<Pages>();
    pages.setGrow(1.0f);
    // The same height for every section, so the dialog doesn't resize when the page changes
    pages.setMinSize({0.0f, pageHeight});
    auto items = std::make_shared<std::vector<ListItem *>>();
    auto names = std::make_shared<std::vector<std::string>>();
    Pages *pagesPtr = &pages;
    auto page = [&nav, &pages, pagesPtr, items, names, pageHeight](const std::string &name) -> Container & {
        const size_t index = items->size();
        names->push_back(name);
        ListItem &item = nav.add<ListItem>(name, [pagesPtr, items, index] {
            pagesPtr->setCurrent(index);
            for (size_t i = 0; i < items->size(); ++i)
                (*items)[i]->setSelected(i == index);
        });
        item.setSelected(index == 0);
        items->push_back(&item);
        auto &scroll = pages.add<ScrollView>();
        scroll.setViewHeight(pageHeight);
        scroll.content().setAlign(Align::Stretch);
        return scroll.content();
    };

    {
        Container &body = page("Audio");
        body.add<Label>("Audio", TextStyle::Subtitle);
        body.add<Label>("Changes reopen the output device. Exclusive mode takes the device from other programs.",
                        TextStyle::Caption, TextTone::Secondary)
            .setWrapWidth(500.0f);

        std::vector<std::string> outputs{"System default"};
        for (const audio::DeviceInfo &d : system.devices())
            outputs.push_back(d.isDefault ? d.name + " (default)" : d.name);
        auto &output = row(body);
        output.add<Label>("Output", TextStyle::Body, TextTone::Secondary).setMinSize({80.0f, 0.0f});
        output.add<Dropdown>(gui_, outputs, system.settings().device + 1, [this, &system](int i) {
            audio::AudioSettings settings = system.settings();
            settings.device = i - 1;
            system.restart(settings);
            // Remembered by name, for next time
            settings_.audioOutput = i > 0 ? system.devices()[static_cast<size_t>(i - 1)].name : std::string();
            std::string error;
            settings_.save(error);
        }).setSize(Size::Small).setGrow(1.0f);

        const std::vector<uint32_t> sizes{64, 128, 256, 512, 1024};
        std::vector<std::string> names;
        int current = 2;
        for (size_t i = 0; i < sizes.size(); ++i) {
            names.push_back(format("%.0f frames", sizes[i]));
            if (sizes[i] == system.settings().bufferFrames)
                current = static_cast<int>(i);
        }
        auto &buffer = row(body);
        buffer.add<Label>("Buffer", TextStyle::Body, TextTone::Secondary).setMinSize({80.0f, 0.0f});
        buffer.add<Dropdown>(gui_, names, current, [this, &system, sizes](int i) {
            audio::AudioSettings settings = system.settings();
            settings.bufferFrames = sizes[static_cast<size_t>(i)];
            system.restart(settings);
            settings_.bufferFrames = settings.bufferFrames;
            std::string error;
            settings_.save(error);
        }).setSize(Size::Small).setGrow(1.0f);

        body.add<Checkbox>("Exclusive mode, for the lowest latency", system.settings().exclusive, [&system](bool on) {
            audio::AudioSettings settings = system.settings();
            settings.exclusive = on;
            system.restart(settings);
        }).setSize(Size::Small);

        body.add<LiveLabel>([&system] {
            if (!system.running())
                return system.lastError().empty() ? std::string("Stopped") : system.lastError();
            return system.deviceName() + format(",  %.0f Hz", system.sampleRate()) +
                   format(",  %.1f ms", system.bufferMs());
        }, TextStyle::Caption, TextTone::Secondary);
    }

    {
        // Inputs: the audio input and MIDI devices
        Container &body = page("Recording");
        body.add<Label>("Recording", TextStyle::Subtitle);
        buildInputSettings(body);
    }

    {
        // The sample library
        Container &body = page("Sample library");
        body.add<Label>("Sample library", TextStyle::Subtitle);
        auto &folder = row(body);
        folder.add<Label>("Folder", TextStyle::Body, TextTone::Secondary).setMinSize({80.0f, 0.0f});
        auto &path = folder.add<TextField>("Library folder");
        path.setText(settings_.libraryPath);
        path.setSize(Size::Small).setGrow(1.0f);
        auto &status = body.add<Label>("", TextStyle::Caption, TextTone::Secondary);
        auto describe = [this] {
            const size_t count = library_->instruments().size();
            const size_t samples = library_->samples().size();
            return format("%.0f instrument", static_cast<double>(count)) + (count == 1 ? "" : "s") +
                   format(" and %.0f sample", static_cast<double>(samples)) + (samples == 1 ? "" : "s") + " in " +
                   settings_.libraryPath;
        };
        status.setText(describe());
        auto &actions = row(body);
        actions.add<Button>([this, &path, &status, describe] {
            settings_.libraryPath = path.text();
            std::string error;
            if (!settings_.save(error)) {
                status.setText(error);
                return;
            }
            library_->rescan();
            status.setText(describe());
        }).setLabel("Use this folder").setSize(Size::Small);
        actions.add<Button>([this, &status, describe] {
            library_->rescan();
            status.setText(describe());
        }).setLabel("Rescan").setVariant(ButtonVariant::Flat).setSize(Size::Small);
        actions.add<Button>([this] { importInstrument(); }).setLabel("Import instrument").setIcon(Icon::Plus).setSize(Size::Small);
    }

    {
        // Threads and sample memory
        Container &body = page("Performance");
        body.add<Label>("Performance", TextStyle::Subtitle);
        buildPerformanceSettings(body);
    }

    {
        // CLAP plugins: the sandbox, the folders scanned, importing, and each plugin
        Container &body = page("Plugins");
        body.add<Label>("Plugins", TextStyle::Subtitle);
        buildPluginSettings(body);
    }

    {
        // Live figures: the audio thread's load, threads, memory, frame times
        Container &body = page("Metrics");
        body.add<Label>("Metrics", TextStyle::Subtitle);
        buildMetrics(body);
    }

    {
        // Where the browser's Online tab gets things
        Container &body = page("Online");
        body.add<Label>("Online", TextStyle::Subtitle);
        body.add<Label>("The browser downloads instruments and samples from these sources. Freesound needs a free API "
                        "key from your Freesound account.",
                        TextStyle::Caption, TextTone::Secondary)
            .setWrapWidth(500.0f);
        auto &keyRow = row(body);
        keyRow.add<Label>("Freesound", TextStyle::Body, TextTone::Secondary).setMinSize({80.0f, 0.0f});
        auto &key = keyRow.add<TextField>("API key");
        key.setPassword(true).setText(settings_.freesoundKey);
        key.setSize(Size::Small).setGrow(1.0f);
        auto &keyStatus = body.add<Label>(settings_.freesoundKey.empty() ? "No key yet" : "Key saved", TextStyle::Caption,
                                          TextTone::Secondary);
        auto &keyActions = row(body);
        keyActions.add<Button>([this, &key, &keyStatus] {
            std::string error;
            settings_.freesoundKey = key.text();
            keyStatus.setText(settings_.save(error) ? (key.text().empty() ? "Key removed" : "Key saved") : error);
        }).setLabel("Save key").setSize(Size::Small);
        keyActions.add<Button>([] { engine::openUrl("https://freesound.org/apiv2/apply"); })
            .setLabel("Get a key")
            .setVariant(ButtonVariant::Flat)
            .setSize(Size::Small);
        auto &sources = row(body);
        std::string sourceNames;
        for (const SourceConfig &c : online_->sources())
            sourceNames += (sourceNames.empty() ? "" : ", ") + c.name;
        const bool custom = std::filesystem::exists(std::filesystem::u8path(sourcesPath()));
        sources.add<Label>("Sources: " + sourceNames + (custom ? " (from sources.toml)" : ""), TextStyle::Caption,
                           TextTone::Secondary)
            .setWrapWidth(380.0f)
            .setGrow(1.0f);
        sources.add<Button>([this] {
            // The defaults written out to start from, the first time
            const std::filesystem::path path = std::filesystem::u8path(sourcesPath());
            std::error_code ec;
            if (!std::filesystem::exists(path, ec)) {
                std::filesystem::create_directories(path.parent_path(), ec);
                std::ofstream out(path, std::ios::binary);
                out << "# Where Vibin's browser finds things to download. Changes take effect when Vibin next starts.\n"
                       "# type = \"github\" (an organisation's repositories, each an SFZ instrument), \"freesound\",\n"
                       "# or \"catalogue\" (a TOML file or web address with [[items]]: name, kind, url, category,\n"
                       "# license, author, page, description, size, preview).\n\n";
                for (const SourceConfig &c : defaultSources()) {
                    out << "[[sources]]\nname = " << toml::quote(c.name) << "\ntype = " << toml::quote(c.type) << "\n";
                    if (!c.org.empty())
                        out << "org = " << toml::quote(c.org) << "\n";
                    if (!c.exclude.empty()) {
                        out << "exclude = [";
                        for (size_t i = 0; i < c.exclude.size(); ++i)
                            out << (i ? ", " : "") << toml::quote(c.exclude[i]);
                        out << "]\n";
                    }
                    out << "\n";
                }
            }
            engine::showInFolder(path.string());
        }).setLabel("Edit sources").setVariant(ButtonVariant::Flat).setSize(Size::Small);
        if (!online_->sourcesError().empty())
            body.add<Label>(online_->sourcesError(), TextStyle::Caption, TextTone::Accent).setWrapWidth(500.0f);
    }

    {
        Container &body = page("Saving");
        buildSavingSettings(body);
    }

    {
        Container &body = page("Keyboard");
        body.add<Label>("Keyboard", TextStyle::Subtitle);
        auto &keys = row(body);
        keys.add<LiveLabel>([this] {
            const size_t changed = ui_.keymap().overrides().size();
            return changed == 0 ? std::string("Every shortcut is on its default keys")
                                : format("%.0f shortcut", static_cast<double>(changed)) + (changed == 1 ? "" : "s") +
                                      " changed from the defaults";
        }, TextStyle::Caption, TextTone::Secondary);
        keys.add<Spacer>();
        keys.add<Button>([this] { showShortcuts(); }).setLabel("Keyboard shortcuts").setSize(Size::Small);
    }

    for (size_t i = 0; i < names->size(); ++i) {
        if ((*names)[i] == section) {
            pages.setCurrent(i);
            for (size_t j = 0; j < items->size(); ++j)
                (*items)[j]->setSelected(j == i);
        }
    }

    dialog.addButton("Stop device", [&system] { system.stopDevice(); });
    dialog.addButton("Restart", [&system] { system.restart(system.settings()); });
    dialog.addButton("Done", {}, true);
}

void DawApp::showShortcuts()
{
    openShortcutsEditor(gui_, ui_.keymap(), [this] {
        std::string error;
        if (!saveKeybinds(ui_.keymap(), error))
            Dialog::open(gui_, "Couldn't save the shortcuts", error).addButton("OK", {}, true);
    });
}

void DawApp::showAppearance()
{
    // A toggle: one window at a time
    ui_.post([this] {
        if (appearance_) {
            appearance_->close();
            return;
        }
        appearance_ = &openAppearance(gui_, themePreset_, [this] { appearance_ = nullptr; });
    });
}

void DawApp::showAbout()
{
    Dialog &dialog = Dialog::open(gui_, "About Vibin",
                                  "A digital audio workstation on a neumorphic UI engine, Vulkan and miniaudio.");
    dialog.body().add<Label>("Inter font, SIL Open Font License", TextStyle::Caption, TextTone::Muted);
    dialog.addButton("Close", {}, true);
}

}
