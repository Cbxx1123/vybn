#pragma once

#include <array>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "daw/app/Appearance.h"
#include "daw/io/Autosave.h"
#include "daw/audio/AudioBridge.h"
#include "daw/audio/AudioPool.h"
#include "daw/model/FoldState.h"
#include "daw/model/History.h"
#include "daw/library/Library.h"
#include "daw/model/NoteTools.h"
#include "daw/views/PianoRollMenus.h"
#include "daw/library/Online.h"
#include "daw/library/PreviewPlayer.h"
#include "daw/audio/Exporter.h"
#include "daw/audio/RecordingController.h"
#include "daw/model/Project.h"
#include "daw/model/ProjectSession.h"
#include "daw/audio/Recording.h"
#include "daw/app/Settings.h"
#include "daw/views/StepSequencer.h"
#include "daw/widgets/Widgets.h"
#include "audio/AudioSystem.h"
#include "audio/ClipPlayer.h"
#include "audio/Midi.h"
#include "audio/NotePlayer.h"
#include "audio/Preview.h"
#include "audio/Recorder.h"
#include "audio/PluginInstrument.h"
#include "audio/Processors.h"
#include "audio/Sampler.h"
#include "audio/Track.h"
#include "env/Environment.h"
#include "plugin/Manager.h"
#include "daw/library/DeviceCatalogue.h"
#include "daw/views/DeviceView.h"
#include "ui/UI.h"

namespace daw {

class AutomationWriter;
class AutosaveController;
class ArrangementView;
class AssetBrowser;
class AudioEditorView;
class DevicesView;
class MixerView;
class PianoRollView;

// The DAW's main window, built into the UISystem's main window: title bar with the menus, the
// transport bar, the inspector beside the arrangement, the mixer or the piano roll under it, and
// a status bar. It owns the project (and the .vbp file it was opened from or saved to), the
// settings, the instrument library and the pool of sound files, and adds the metronome, the MIDI
// hub, the input recorder and a TrackProcessor per track to the audio graph. An instrument track's
// processor plays its notes through a NotePlayer and a Sampler loaded with the track's library
// instrument, and plays MIDI keyboards while armed (or selected, when none is armed); an audio
// track's plays its clips through an AudioClipPlayer and can monitor the input. MIDI inputs plugged
// in are opened as they appear (unless turned off in Settings); the audio input opens the first
// time an audio track is armed. Build it after the daemons are added and before
// Environment::run; it must outlive the run.
class DawApp {
public:
    DawApp(env::Environment &env, ui::UISystem &ui, audio::AudioSystem &audio);
    // Removes the session lock: a clean exit (see AutosaveController)
    ~DawApp();

    DawApp(const DawApp &) = delete;
    DawApp &operator=(const DawApp &) = delete;

    // The store every change to the project goes through (see ProjectStore)
    ProjectStore &store() { return session_.store(); }
    // The project as it is now, for reading
    const Project &project() const { return session_.store().project(); }
    ui::MenuBar &menuBar() { return *menuBar_; }

    // Project > New, Open, Save and Save as. New and Open ask about unsaved changes first. Open
    // and Save as show the system's file dialogs; Save asks for a file only the first time.
    void newProject();
    void openProject();
    void saveProject();
    void saveProjectAs();
    // Opens a .vbp file without asking about unsaved changes; returns false (showing why) on failure
    bool openProjectFile(const std::string &path);
    // Whether the project differs from the file it was opened from or last saved to
    bool modified() const { return session_.modified(); }
    const std::string &projectPath() const { return session_.path(); }
    // Asks about unsaved changes, then quits
    void quit();
    // Start-up, once the window shows: a session that ended without closing is offered for recovery
    void offerRecovery();
    // Project > Versions (the project's autosaves and saved versions) and Project > Save version...
    void openVersions();
    void openSaveVersion();
    // The --demo versions check: two manual versions of the project, then the Versions dialog
    void versionsDemo();

    void addTrack(TrackKind kind);
    // A new group track just before `index`, going where the track went, and the track going into it
    void groupTrack(int index);
    // Removes a track (after asking, if it has clips); not while recording
    void deleteTrack(int index);
    // Settings, on `section` ("Plugins") or the first
    void openSettings(const std::string &section = {});
    // For screenshots: once the plugins are scanned, the first instrument plugin installed (else
    // the test sine) plays the Lead track, with its interface open inside the app
    void pluginEditorDemo();
    // Name, tempo, time signature and length
    void openProjectSettings();
    // Asks for files with the system's open dialog and copies them into the library
    void importInstrument();
    // Asks for sound files and places them at the playhead: on the selected audio track (or one
    // made for them), one after another
    void importAudio(int track = -1);
    void importAudioFiles(const std::vector<std::string> &files, int track = -1);
    // A generated take imported onto the first audio track and opened in the editor (--demo)
    void importAudioDemo();
    // Records a phrase sent through the MIDI hub onto the Lead track, as if played on a
    // keyboard, then opens the take in the piano roll (--demo recordmidi)
    void recordMidiDemo();
    // Starts recording onto the armed tracks, or stops (see RecordingController)
    void toggleRecording() { recordingController_->toggleRecording(); }
    bool recording() const { return recording_->recording(); }
    // Cuts the selected clip at the playhead
    void splitSelectedClip();
    // Shows the clip editor (the piano roll, or the audio editor for an audio clip), or the
    // mixer, under the arrangement
    void showEditor();
    // Gives the editor's place this share of its split (the demos, for room)
    void setEditorShare(float share);
    // The piano roll's view as it is now counts as saved (a demo's changes aren't the user's)
    void keepRollView();
    // The step sequencer, the editor's Steps page for instrument clips (View > Step sequencer, Ctrl+5)
    StepSequencerView &steps() { return *steps_; }
    void setStepMode(bool on);
    bool stepMode() const { return stepMode_; }
    // The --demo steps check: a drum beat on a Drums track, in the step sequencer with its kick row selected
    void stepsDemo();
    void masterAutomationDemo();
    void foldingDemo();
    // --demo sidechain: a pad with a compressor keyed by a muted kick track, the Devices tab showing it
    void sidechainDemo();
    // --demo flexible: three tracks into Group 1, one also sending to Group 2, the mixer showing
    void flexibleDemo();
    void showMixer();
    void showEffects();
    void showExports();
    // The selected clip's name, start (bar and beat) and length
    void openClipDialog();
    // The shortcuts editor; changes are saved to keybinds.toml as they're made
    void showShortcuts();
    void showAbout();
    // View > Appearance: opens the theme customiser (saved to theme.toml), or closes it if open
    void showAppearance();
    bool appearanceOpen() const { return appearance_ != nullptr; }
    // The asset browser (Tab): open or put away
    void toggleBrowser();
    AssetBrowser &browser() { return *browser_; }
    PianoRollView &pianoRoll() { return *pianoRoll_; }
    DeviceCatalogue &catalogue() { return *catalogue_; }
    // The device picker: an instrument for `track` (a non-instrument track or -1: a new instrument
    // track), or an effect added to the master's chain or the track's, opened beside `anchor`
    // (window pixels; an empty one: near the top middle of the window)
    void pickDevice(bool instrument, bool master, int track, ui::Rect anchor = {});
    // The picker on the selected track's instrument
    void chooseInstrument();
    // The one way a track gets an instrument: `device` on `track`, selected. A track that isn't an
    // instrument track (or -1) gets a new instrument track, named after the device.
    void setInstrument(int track, const audio::DeviceSetting &device);
    // A catalogue entry by key ("sampler:<name>", "plugin:<id>") as the selected instrument
    // track's instrument, or a new track's
    void useDevice(const std::string &key);
    // Dragging an instrument from the browser: the track a drop at `at` would set (the track
    // count for a new one, -1 for none), and the drop itself (`key` a catalogue key)
    int instrumentDropRow(ui::Vec2 at) const;
    void dropInstrument(const std::string &key, int row);
    // A device's full view in a dock panel of its own, floating at first (one per place; asking
    // again shows it), on its plugin's interface when `interface` and it has one
    void openDevice(const DevicePlace &place, bool interface = false);
    // The place as it is now: a view's track is found again by its id (-1 if it was deleted)
    DevicePlace placeAt(const DevicePlace &place, ObjectId track) const;
    // Puts a sound file on the selected audio track (or a new one) at the playhead
    void addSound(const std::string &path);
    // Asks for sound files and copies them into the library's Samples/<category>
    void importSamples(const std::string &category);
    // Listening in the browser: a phrase on an instrument, or a sound file, without the transport
    void previewInstrument(const std::string &name);
    void previewSound(const std::string &path);
    void previewSynth(const audio::DeviceSetting &setting);
    void stopPreview();
    bool previewing() const;
    void setDarkMode(bool dark);
    // Edit > Undo and Redo: every change to the project, wherever it was made (see History)
    void undo();
    void redo();
    const History &history() const { return session_.history(); }
    // Project > Export: the export dialog, which queues a preset's render (see Exporter)
    void openExportDialog() { exporter_->openDialog(); }
    // The --demo exportmix check: the project's Mix preset rendered into the temp folder
    void exportDemo() { exporter_->runDemo(); }
    // The --demo exportrelease check: the project's Release preset rendered into the temp folder
    void exportReleaseDemo() { exporter_->runReleaseDemo(); }
    // --silent: exports render at unity master gain (see Exporter::setSilentRun)
    void setSilentRun(bool silent) { exporter_->setSilentRun(silent); }

private:
    void buildTitleBar();
    void buildBody();
    void buildMenus();
    void buildBrowser();
    // Sets the transport's loop, then has the AudioBridge bring the audio side in step with the
    // project (tracks, mix, notes, instruments). First each frame.
    void syncTracks();
    // Swaps in another project: stops the transport, takes the old tracks out of the audio graph
    // and rebuilds the views. Changes the tree, so only from a posted action.
    void replaceProject(Project project, std::string path, const Project *saved = nullptr);
    // Puts back a state from the history: the model, the audio graph and the views. From a
    // posted action.
    void restore(HistoryRestore plan);
    // Runs then, after offering to save unsaved changes (Cancel runs nothing)
    void confirmDiscard(std::function<void()> then);
    // Saves to the project's path, or asks for one with the save dialog when it has none or
    // asking is set. Runs after on success (not when the dialog is cancelled).
    void save(bool ask, std::function<void()> after = {});
    void saveTo(const std::string &path, std::function<void()> after);
    // The folder the file dialogs start in: the current project's, else the library's Projects
    std::string projectsFolder() const;
    // Opens a file dialog: the app's own (ui::FileDialog, with the project's folder and the
    // library among its places), or Windows' when Settings asks for it. `done` runs after the
    // frame with what was picked, and only when something was.
    void chooseFiles(ui::FileDialogOptions options, ui::FileDialog::Done done);
    // Mentions library instruments the project plays that aren't installed
    void reportMissingInstruments();
    // Saving (DawSaving.cpp): recovering, discarding or declining a session that ended without closing,
    // the Versions dialog for a folder of versions, opening or restoring one, and the autosave settings
    void recoverSession();
    void discardSession();
    void declineRecovery();
    void showRecoverableVersions();
    void openVersionsFor(const std::string &folder, const std::string &path, const std::string &name);
    void openVersionCopy(const std::string &folder, const AutosaveVersion &version, const std::string &name);
    void restoreVersion(const std::string &folder, const AutosaveVersion &version, const std::string &path);
    void confirmDeleteVersion(const std::string &folder, const AutosaveVersion &version, const std::string &path,
                              const std::string &name);
    // Autosave's interval in seconds (Settings, or VYBN_AUTOSAVE_SECONDS for tests)
    double autosaveInterval() const;
    // Pushes the settings' autosave choices to the controller and saves them
    void applyAutosaveSettings();
    // Settings > Saving: autosave, its interval and versions kept, and the autosave folder
    void buildSavingSettings(ui::Container &body);
    // The new-project dialog: settings and starting tracks, then Create
    void openNewProjectDialog();

    // Inputs (DawAudio.cpp)
    // Opens MIDI inputs that appeared (every two seconds) and the audio input once it's needed
    void syncInputs(bool force = false);
    // (Re)opens the audio device with the input from the settings
    void openAudioInput();
    // The settings' devices as the AudioSystem's
    void applyAudioSettings();
    int outputIndex() const;
    int inputIndex() const;
    std::string midiInputText() const;
    std::string audioInputText() const;
    std::string trackInputText(int track) const;
    void notify(std::string message);
    std::string notice() const;
    // An audio track's clip's sound, from the recording in progress or the pool
    const audio::AudioData *clipAudio(int track, int clip);
    void openClipMenu(ui::Vec2 at, double beat);
    void openTrackMenu(int track, ui::Vec2 at);
    // What a track can automate: its volume and pan, then each of its devices' parameters (a
    // submenu each). Picking one adds its lane (empty, so it plays nothing yet) and shows the lanes.
    void openAutomationMenu(int track, ui::Vec2 at);
    void openMasterAutomationMenu(ui::Vec2 at);
    void buildInputSettings(ui::Container &body);
    // Settings > Performance: audio threads, streaming samples and its memory, and what the
    // scheduler and memory manager are doing
    void buildPerformanceSettings(ui::Container &body);
    // Settings > Metrics: the audio device and its load, threads, sample memory and frame times
    void buildMetrics(ui::Container &body);

    // Settings > Plugins: the sandbox, folders, importing, and each plugin (DawPlugins.cpp)
    void buildPluginSettings(ui::Container &body);
    // The settings' plugin folders, sandbox and per-plugin choices into the PluginManager, and saved
    void applyPluginSettings();
    // Every place plugin `id` plays (all of them for an empty id) starts a new host
    void reloadPlugin(const std::string &id);
    // Copies .clap files (picked in the file dialog) into the imported plugins' folder and scans
    void importPlugin();
    env::Environment &env_;
    ui::UISystem &ui_;
    audio::AudioSystem &audio_;
    ui::Context &gui_;
    // The document: the project, its .vbp file, whether it's saved, and its undo history
    ProjectSession session_;
    Settings settings_;
    // Autosave of the project and the session lock (Autosave.h)
    std::unique_ptr<AutosaveController> autosave_;
    // A session that ended without closing, found at start-up (found is false when there is none)
    Recoverable recoverable_;
    std::string rollViewSaved_; // the piano roll's view as last saved in the settings
    plugin::PluginManager *plugins_ = nullptr; // the environment's, if it has one
    std::vector<std::string> extraPluginFolders_; // scanned this run only (--plugin-folder)
    // Everything that can go in a chain (the library's instruments, built-in effects, plugins)
    std::unique_ptr<DeviceCatalogue> catalogue_;
    // Open device views, by place: each a closable dock panel (floating in a native window, or
    // docked into the grid), kept while the place holds the same device
    struct OpenDevice {
        ui::DockPanel *panel;
        DeviceView *view;
        std::string key; // the device's (DeviceSetting::key) when opened
        DevicePlace place;
        ObjectId track = kNoId; // the place's track by id (kNoId for the master): its index moves
        bool closing = false; // its device left its place: being taken out
    };
    std::map<std::string, OpenDevice> deviceViews_;
    std::unique_ptr<InstrumentLibrary> library_;
    std::unique_ptr<OnlineCatalogue> online_;
    std::unique_ptr<Downloads> downloads_;
    StereoLevels levels_;
    audio::Metronome &metronome_;
    audio::MidiHub &midi_;          // first in the graph after the metronome, before every track
    audio::InputRecorder &recorder_;
    std::unique_ptr<AudioPool> pool_;
    // The browser's listening previews, outside the project (see PreviewPlayer)
    std::unique_ptr<PreviewPlayer> preview_;
    std::unique_ptr<RecordingSession> recording_;
    // The audio side of the project: the track processors and their players (see AudioBridge)
    std::unique_ptr<AudioBridge> bridge_;
    // Starts and stops takes, and the count-in (see RecordingController)
    std::unique_ptr<RecordingController> recordingController_;
    // Writes what armed tracks' controls do into their automation while the song plays (see AutomationWriter)
    std::unique_ptr<AutomationWriter> automationWriter_;
    // The mix export: its dialogs and its render, stepped each frame (see Exporter)
    std::unique_ptr<Exporter> exporter_;
    // Inputs
    bool inputTried_ = false;               // the lazy audio input has been opened (or failed)
    std::set<std::string> midiFailed_;      // MIDI inputs that wouldn't open, not retried
    uint64_t midiSeen_ = 0;                 // MIDI messages seen, for the activity light
    double midiTime_ = -10.0;               // when the last one arrived
    std::array<float, audio::AudioSystem::kMaxInputs> inputPeaks_{}; // this frame's
    std::string notice_;
    double noticeUntil_ = 0.0;
    std::shared_ptr<std::function<void()>> demoTick_; // recordMidiDemo's, run every frame
    AssetBrowser *browser_ = nullptr;
    bool mouseSinceStep_ = false; // the mouse was used since the last undo step (it isn't merged)
    bool mouseHeld_ = false;      // a button is held (undo waits for the drag to end)
    bool restoring_ = false;      // an undo or redo waits to be put back (the history holds still)
    bool typing_ = false;         // a text field has the keyboard (its Ctrl+Z isn't the project's)
    std::string themePreset_ = "Carbon"; // the Appearance preset the theme started from
    std::string themeSaved_;             // the theme as last written to theme.toml
    // The window's icon is made from the theme's base and accent colours (AppIcon.h): the ones it was made
    // from, so it is made again only when the look changes
    ui::Color iconBase_{}, iconAccent_{};
    bool iconSet_ = false;
    void refreshWindowIcon();
    ui::FloatingWindow *appearance_ = nullptr; // the Appearance window while it is open
    ui::MenuBar *menuBar_ = nullptr;
    FoldState folds_{session_.store()}; // folding groups (Track::folded), for the arrangement and the mixer
    ArrangementView *arrangement_ = nullptr;
    MixerView *mixer_ = nullptr;
    PianoRollView *pianoRoll_ = nullptr;
    StepSequencerView *steps_ = nullptr; // the step sequencer, the editor's third page
    bool stepMode_ = false;              // the editor shows the step sequencer for instrument clips
    std::string stepViewSaved_;          // the step sequencer's view as last saved in the settings
    std::unique_ptr<PianoRollMenus> rollMenus_; // the piano roll's menus, made with the roll
    AudioEditorView *audioEditor_ = nullptr;
    ui::Slider *clipGain_ = nullptr;
    ui::DockSpace *dock_ = nullptr; // the arrangement, mixer and editor panels
    ui::DockPanel *arrangementPanel_ = nullptr;
    ui::DockPanel *mixerPanel_ = nullptr;
    ui::DockPanel *editorPanel_ = nullptr;
    ui::DockPanel *effectsPanel_ = nullptr;
    ui::DockPanel *exportsPanel_ = nullptr;
    DevicesView *effects_ = nullptr;
    ui::Pages *editors_ = nullptr; // the piano roll, the audio editor and the step sequencer
};

}
