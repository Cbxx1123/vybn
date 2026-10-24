// The DAW. An env::Environment runs the program's daemons: the AudioSystem (the audio device and
// its real-time thread), the WindowSystem (native windows and their renderers) and the UISystem
// (a UI for each window). daw::DawApp builds the DAW's window into the UISystem.
//
//   main [--dark] [--silent] [--size WxH] [--demo NAME] [PROJECT.vbp]
//   main --audio-selftest   checks the audio system offline (no device, no sound) and exits
//   main --project-selftest checks the TOML reader and the .vbp project format and exits
//   main --input-selftest   checks the input signals, the keymap and text editing and exits
//   main --engine-selftest  checks the scheduler, the real-time pool, parallel mixing and
//                           streaming samples from disk offline and exits
//   main --notes-selftest   checks the piano roll's note tools (scales, chords, arpeggiator,
//                           quantize, velocity...) offline and exits
//   main --browser-selftest checks the browser's listings, zip files, the SFZ loader and the
//                           library offline (no network) and exits
//   main --plugin-selftest  checks plugins, CLAP and VST3, through the sandbox (the test plugins:
//                           audio, parameters, state, notes, and surviving crashes and hangs)
//   main --synth-selftest   checks the built-in synthesizer offline (the wave bank, pitch, envelopes,
//                           filters, unison, voice modes, LFOs, the mod matrix and render time)
//   main --export-selftest  checks the export's mastering tools offline (loudness, true peak,
//                           the true-peak limiter, dither, fades) and exits
//   main --render-selftest  renders the UI's and the DAW's scenes off screen and compares them with
//                           their golden pictures (--update-goldens writes them, --scene NAME renders one)
//   main --import FILE...   copies an instrument (an .sfz with its samples, or note-named audio
//                           files) into the sample library and exits
//     --dark       start in dark mode
//     --silent     start with the master gain at zero (the track meters still move)
//     --size WxH   start with a window of this size (default 1440x900)
//     --plugin-folder PATH  also scan this folder for plugins, this run only (repeatable)
//     --demo NAME  start with something open, for screenshots: menu (the Edit menu), project
//                  (the Project menu), new (the new project dialog), projectsettings, audioclip (a
//                  generated test tone imported onto an audio track, in the editor), warp (a generated
//                  96 BPM beat warped into the song, its transients pinned, in the editor), freeze (the
//                  first track frozen and the second bounced), analyzer (the Analyzer panel while the
//                  transport plays), recordmidi (records a
//                  phrase sent through the MIDI hub onto the Lead track), browser (the asset
//                  browser on the library's instruments), appearance (the theme customiser), notetools (the piano roll in A minor with notes
//                  selected, a muted beat, ghosts and a loop), unsaved (New with an
//                  unsaved change: the save prompt), settings, shortcuts, clip (selects a clip),
//                  pianoroll (the first guitar clip in the piano roll), or transport (plays the
//                  transport: the guitar tracks are heard), synth (the Lead track played by the
//                  built-in synthesizer, the Devices tab showing), exportmix (the Mix preset
//                  rendered into the temp folder, without asking), exportrelease (the Release preset
//                  rendered into the temp folder and the Exports panel shown), export (the export dialog),
//                  versions (two saved versions, "First idea" and "With bass", and the Versions dialog),
//                  recovery (a session that ended without closing, and the recovery dialog over the window),
//                  steps (a drum beat on a Drums track in the step sequencer, its kick row selected),
//                  masterautomation (the arrangement's master row with its volume and a limiter's lanes
//                  shown, its writing armed, and the transport playing),
//                  folding (two nested groups round some tracks, the inner one folded, playing),
//                  sidechain (a pad whose compressor is keyed by a muted kick track, the Devices tab
//                  showing it, playing), pluginsidechain (the same with the test plugin Test Duck keyed
//                  by the kick; with --plugin-folder output/plugins),
//                  flexible (three tracks into Group 1, one also sending to Group 2 and a return, the
//                  mixer showing, playing).
//                  Both autosave into a fresh temp folder unless VYBN_AUTOSAVE_ROOT names one.
//     PROJECT.vbp  open this project instead of the demo

#include <algorithm>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <functional>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "AudioSelfTest.h"
#include "BrowserSelfTest.h"
#include "EngineSelfTest.h"
#include "InputSelfTest.h"
#include "NoteSelfTest.h"
#include "PluginSelfTest.h"
#include "SynthSelfTest.h"
#include "ExportSelfTest.h"
#include "ProjectSelfTest.h"
#include "RenderSelfTest.h"
#include "audio/AudioSystem.h"
#include "audio/Memory.h"
#include "daw/io/Autosave.h"
#include "daw/app/DawApp.h"
#include "daw/views/PianoRoll.h"
#include "daw/library/Library.h"
#include "daw/app/Settings.h"
#include "daw/app/AppIcon.h"
#include "daw/app/Splash.h"
#include "daw/app/Appearance.h"
#include "engine/WindowSystem.h"
#include "env/Environment.h"
#include "env/Resources.h"
#include "env/Scheduler.h"
#include "audio/Synth.h"
#include "plugin/Manager.h"
#include "ui/UI.h"

// --demo versions and recovery keep their autosaves in a folder of their own (unless
// VYBN_AUTOSAVE_ROOT names one), so the user's own are never touched. Recovery also leaves behind a
// session that ended without closing: the demo project with a changed tempo, autosaved ten minutes
// ago by a process that is no longer running.
void prepareAutosaveDemo(const std::string &demo)
{
    namespace fs = std::filesystem;
    const char *set = std::getenv("VYBN_AUTOSAVE_ROOT");
    std::string root = set ? set : "";
    if (root.empty()) {
        root = (fs::temp_directory_path() / ("vybn-demo-" + demo + "-" + std::to_string(daw::currentProcessId()))).string();
        std::error_code ec;
        fs::remove_all(root, ec);
        fs::create_directories(root, ec);
#ifdef _WIN32
        _putenv_s("VYBN_AUTOSAVE_ROOT", root.c_str());
#endif
    }
    if (demo != "recovery")
        return;
    const double now = daw::unixSeconds();
    const std::string key = "crashed-demo";
    daw::Project project = daw::demoProject();
    project.timing.setTempo(96.0);
    std::string file, error;
    daw::writeAutosaveVersion((fs::path(root) / key).string(), "", project, now - 600.0, false, "", 10, file, error);
    daw::SessionLock lock;
    lock.pid = 999999; // no such process
    lock.started = now - 1200.0;
    lock.key = key;
    daw::writeSessionLockFile(root, lock, error);
}

// A plugin device for the demos
audio::DeviceSetting pluginDevice(const std::string &id, const std::string &name)
{
    audio::DeviceSetting d;
    d.kind = audio::DeviceKind::Plugin;
    d.id = id;
    d.name = name;
    return d;
}

int main(int argc, char **argv)
{
    bool dark = true;
    bool forceMode = false; // --dark or --light: over the saved look
    bool silent = false;
    unsigned long width = 1440;
    unsigned long height = 900;
    std::string demo;
    std::string projectFile;
    std::vector<std::string> pluginFolders; // --plugin-folder: scanned this run, besides the settings'
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--audio-selftest")
            return tests::runAudioSelfTest();
        if (arg == "--project-selftest")
            return tests::runProjectSelfTest();
        if (arg == "--input-selftest")
            return tests::runInputSelfTest();
        if (arg == "--browser-selftest")
            return tests::runBrowserSelfTest();
        if (arg == "--notes-selftest")
            return tests::runNoteSelfTest();
        if (arg == "--engine-selftest")
            return tests::runEngineSelfTest();
        if (arg == "--plugin-selftest")
            return tests::runPluginSelfTest();
        if (arg == "--synth-selftest")
            return tests::runSynthSelfTest();
        if (arg == "--export-selftest")
            return tests::runExportSelfTest();
        if (arg == "--render-selftest")
            return tests::runRenderSelfTest(argc, argv);
        if (arg == "--write-icon" && i + 1 < argc) {
            // The executable's icon (assets/icon/vybn.ico, embedded by src/win/vybn.rc), in the default look
            const ui::Theme look = daw::studioTheme(true);
            std::string error;
            if (!daw::writeIconFile(argv[i + 1], look.baseColor(), look.accentColor(), error)) {
                std::cerr << "Couldn't write the icon: " << error << std::endl;
                return 1;
            }
            std::cout << "Wrote " << argv[i + 1] << std::endl;
            return 0;
        }
        if (arg == "--import") {
            daw::Settings settings = daw::Settings::load();
            env::Environment environment;
            daw::InstrumentLibrary library(environment, settings);
            std::string error;
            const std::string name = library.import(std::vector<std::string>(argv + i + 1, argv + argc), error);
            if (name.empty()) {
                std::cerr << "Import failed: " << error << std::endl;
                return 1;
            }
            std::cout << "Imported \"" << name << "\" into " << library.instrumentsFolder() << std::endl;
            return 0;
        }
        if (arg == "--dark" || arg == "--light") {
            dark = arg == "--dark";
            forceMode = true;
        }
        else if (arg == "--silent")
            silent = true;
        else if (arg == "--demo" && i + 1 < argc)
            demo = argv[++i];
        else if (arg == "--plugin-folder" && i + 1 < argc)
            pluginFolders.push_back(std::filesystem::absolute(std::filesystem::u8path(argv[++i])).lexically_normal().u8string());
        else if (arg == "--size" && i + 1 < argc) {
            char *end = nullptr;
            width = std::max(960ul, std::strtoul(argv[++i], &end, 10));
            if (end && *end == 'x')
                height = std::max(640ul, std::strtoul(end + 1, nullptr, 10));
        } else if (arg.size() > 4 && arg.compare(arg.size() - 4, 4, ".vbp") == 0)
            projectFile = arg;
    }

    // The console window this program may have started with stays off the taskbar, so the taskbar button
    // is the app's window and its icon (see AppIcon.h)
    daw::keepConsoleOffTaskbar();

    try {
        // The loading screen first, in the user's look, so nothing shows blank while the app loads.
        // It draws on a thread of its own, and fades once the main window is on screen.
        ui::Theme look = daw::studioTheme(dark);
        {
            std::string preset, error;
            daw::loadTheme(look, preset, error);
            if (forceMode)
                look.setDarkMode(dark);
        }
        daw::Splash splash(look);

        // The daemons, dependencies first. The scheduler owns every worker thread (the audio
        // pool's too), so it goes first and outlives them all. Audio comes next so it outlives the
        // UI that shows it, then the memory manager, whose clock is the audio callbacks. The UI
        // draws into the windowing system's windows.
        daw::Settings::moveFromOldName(); // the user's folders from before the rename, once
        const daw::Settings prefs = daw::Settings::load();
        env::Environment environment;
        auto &scheduler = environment.add<env::Scheduler>();
        if (prefs.audioThreads >= 0)
            scheduler.setRealtimeWorkers(static_cast<uint32_t>(prefs.audioThreads));
        // The program's loaded instruments and sound files: what nothing uses goes after a while
        env::ResourceConfig resources;
        resources.idleSeconds = prefs.unusedSeconds < 0 ? 1e12 : prefs.unusedSeconds;
        environment.add<env::ResourceManager>(resources);
        // Plugins, before the audio system: its effects and instruments hold them. Each runs in a
        // sandboxed process of its own; the folders are scanned in the background.
        plugin::PluginConfig pluginConfig = prefs.pluginConfig();
        pluginConfig.folders.insert(pluginConfig.folders.end(), pluginFolders.begin(), pluginFolders.end());
        auto &plugins = environment.add<plugin::PluginManager>(pluginConfig);
        plugins.rescan();
        auto &audio = environment.add<audio::AudioSystem>();
        audio::MemoryConfig memory;
        memory.streaming = prefs.streamSamples;
        memory.budgetBytes = static_cast<uint64_t>(prefs.streamMemoryMB) << 20;
        memory.cacheFolder = daw::Settings::sampleCachePath();
        environment.add<audio::MemoryManager>(memory);
        engine::Renderer::setOverlaysAllowed(prefs.graphicsOverlays);
        environment.add<engine::WindowSystem>();
        auto &ui = environment.add<ui::UISystem>("VYBN", static_cast<uint32_t>(width), static_cast<uint32_t>(height),
                                                 daw::studioTheme(dark), true);

        if (demo == "versions" || demo == "recovery")
            prepareAutosaveDemo(demo); // before the app reads its autosave folder
        daw::DawApp app(environment, ui, audio);
        // The spare windows floating panels open in, made behind the loading screen rather than
        // as the app starts
        ui.prepareSpares();
        // The main window shows once it has drawn; the loading screen fades then
        ui.onShown([&splash, &app] {
            splash.close();
            app.offerRecovery(); // a session that ended without closing, after the first frames
        });
        if (forceMode)
            app.setDarkMode(dark);
        if (silent) {
            audio.masterGain().set(0.0f);
            app.setSilentRun(true);
        }
        // Replacing the project changes the tree, so after the first frame
        if (!projectFile.empty())
            ui.post([&app, projectFile] { app.openProjectFile(projectFile); });

        // Start-up demos, for screenshots. They need a laid-out frame first, so run after it.
        if (!demo.empty()) {
            ui.post([&app, &audio, &ui, demo] {
                if (demo == "menu")
                    app.menuBar().openMenu(2);
                else if (demo == "project")
                    app.menuBar().openMenu(0);
                else if (demo == "new")
                    app.newProject(); // nothing changed, so straight to the new project dialog
                else if (demo == "projectsettings")
                    app.openProjectSettings();
                else if (demo == "audioclip")
                    app.importAudioDemo();
                else if (demo == "warp")
                    app.warpDemo();
                else if (demo == "freeze")
                    app.freezeDemo();
                else if (demo == "analyzer") {
                    app.showAnalyzer();
                    audio.transport().play();
                }
                else if (demo == "recordmidi")
                    app.recordMidiDemo();
                else if (demo == "unsaved") {
                    app.store().editMix(0).gain = 0.5f;
                    app.newProject(); // asks about the change
                }
                else if (demo == "settings")
                    app.openSettings();
                else if (demo == "pluginsettings")
                    app.openSettings("Plugins");
                else if (demo == "plugineditor")
                    app.pluginEditorDemo();
                else if (demo == "pianoroll") {
                    app.store().select(0, 0);
                    app.showEditor();
                    app.setEditorShare(0.8f);
                }
                else if (demo == "shortcuts")
                    app.showShortcuts();
                else if (demo == "transport")
                    audio.transport().play();
                else if (demo == "takes")
                    app.takesDemo();
                else if (demo == "drop")
                    app.dropDemo();
                else if (demo == "tempo") {
                    // A ramp from 100 to 140 BPM over four bars, held, then 90; 7/8 from bar 5, 6/8 from
                    // bar 7; the transport playing through the changes
                    audio::TempoMap timing = app.store().project().timing;
                    timing.setPoints({{0.0, 100.0, true}, {16.0, 140.0, false}, {32.0, 90.0, false}});
                    timing.setSignatures({{0, 4, 4}, {4, 7, 8}, {6, 6, 8}});
                    app.store().setTiming(timing);
                    audio.transport().play();
                }
                else if (demo == "clip")
                    app.store().select(0, 1);
                else if (demo == "notetools") {
                    // The first guitar clip in the piano roll, in A minor, its first bar selected
                    app.store().select(0, 0);
                    app.showEditor();
                    app.setEditorShare(0.8f);
                    daw::PianoRollView &roll = app.pianoRoll();
                    roll.setScale(1);
                    roll.setRoot(9);
                    for (daw::Note &n : app.store().editClip(0, 0).notes) {
                        n.selected = n.start < 4.0;
                        n.muted = n.start >= 12.0 && n.start < 13.0; // a muted beat
                    }
                    // Every instrument track's notes as ghosts, and the first two bars looping
                    roll.setGhosts(daw::PianoRollView::Ghosts::All);
                    roll.setLoop(0.0, 8.0);
                    roll.setLooping(true);
                    app.keepRollView();
                }
                else if (demo == "steps")
                    app.stepsDemo();
                else if (demo == "folding") {
                    app.foldingDemo();
                    audio.transport().play();
                }
                else if (demo == "sidechain") {
                    app.sidechainDemo();
                    audio.transport().play();
                }
                else if (demo == "flexible") {
                    app.flexibleDemo();
                    audio.transport().play();
                }
                else if (demo == "pluginsidechain") {
                    // The sidechain demo with Test Duck (a plugin with a sidechain port; with --plugin-folder
                    // output/plugins) on the pad in place of the compressor, keyed by the kick
                    app.sidechainDemo();
                    const int pad = static_cast<int>(app.project().tracks.size()) - 2;
                    const daw::ObjectId kick = app.project().tracks.back().id;
                    audio::DeviceSetting duck = pluginDevice("vybn.test.duck", "Test Duck");
                    duck.sidechain = kick;
                    app.store().editTrack(pad).effects = {duck};
                    audio.transport().play();
                }
                else if (demo == "masterautomation") {
                    app.masterAutomationDemo();
                    audio.transport().play();
                }
                else if (demo == "effects") {
                    // A compressor, an EQ and a reverb on the first track, and a limiter on the master
                    app.store().select(0);
                    std::vector<audio::DeviceSetting> chain = {audio::effectSetting(audio::EffectType::Compressor),
                                                               audio::effectSetting(audio::EffectType::Equalizer),
                                                               audio::effectSetting(audio::EffectType::Reverb)};
                    chain[1].setValue(3, 4.5);
                    chain[2].bypass = true;
                    app.store().editTrack(0).effects = chain;
                    app.store().editMasterEffects() = {audio::effectSetting(audio::EffectType::Limiter)};
                    app.showEffects();
                }
                else if (demo == "plugins") {
                    // Plugin effects on the first track (with --plugin-folder output/plugins: the
                    // test plugins), a built-in one between them, and the Devices tab showing. The
                    // faulty one is set to crash, so its card shows how a crash is reported.
                    app.store().select(0);
                    std::vector<audio::DeviceSetting> chain = {pluginDevice("vybn.test.gain", "Test Gain"),
                                                               audio::effectSetting(audio::EffectType::Equalizer),
                                                               pluginDevice("vybn.test.faulty", "Test Faulty")};
                    chain[0].setValue(1, -4.5);
                    chain[0].setValue(4, 128.0); // its latency: the other tracks are held back to match
                    chain[2].setValue(1, 1.0);   // its Fault: crash at its first block
                    app.store().editTrack(0).effects = chain;
                    app.showEffects();
                }
                else if (demo == "plugininstrument") {
                    // The test sine plugin playing the Lead track, its card on the Devices tab
                    audio::DeviceSetting instrument = pluginDevice("vybn.test.sine", "Test Sine");
                    instrument.setValue(1, 0.35);
                    app.store().editTrack(2).instrument = instrument;
                    app.store().editTrack(2).effects = {pluginDevice("vybn.test.gain", "Test Gain")};
                    app.store().select(2);
                    app.showEffects();
                }
                else if (demo == "synth") {
                    // The built-in synthesizer playing the Lead track, at its initial values, and the Devices tab showing
                    audio::DeviceSetting instrument = audio::SynthDevice::setting();
                    instrument.name = "VYBN Synth";
                    app.store().editTrack(2).instrument = instrument;
                    app.store().select(2);
                    app.showEffects();
                }
                else if (demo == "picker") {
                    // The device picker choosing the Lead track's instrument
                    app.store().select(2);
                    app.showEffects();
                    app.pickDevice(true, false, 2);
                }
                else if (demo == "device") {
                    // The first track's EQ in its own view
                    app.store().select(0);
                    app.store().editTrack(0).effects = {audio::effectSetting(audio::EffectType::Equalizer)};
                    app.showEffects();
                    app.openDevice(daw::DevicePlace::effect(false, 0, 0));
                }
                else if (demo == "appearance")
                    app.showAppearance();
                else if (demo == "export")
                    app.openExportDialog();
                else if (demo == "versions")
                    app.versionsDemo();
                else if (demo == "recovery") {
                    // The session was written by prepareAutosaveDemo; the window shows its recovery dialog
                }
                else if (demo == "open")
                    app.openProject(); // nothing changed, so straight to the file dialog
                else if (demo == "saveas")
                    app.saveProjectAs();
                else if (demo == "importaudio")
                    app.importAudio();
                else if (demo == "exportmix")
                    app.exportDemo(); // the Mix preset into the temp folder, without asking where
                else if (demo == "exportrelease")
                    app.exportReleaseDemo(); // the Release preset's targets into the temp folder, queued
                else if (demo == "undo") {
                    // Deletes the first clip's notes, then undoes it a few frames later (once the
                    // edit is a step), and opens the Edit menu to show what Redo would do
                    app.store().select(0, 0);
                    app.showEditor();
                    app.store().editClip(0, 0).notes.clear();
                    auto frames = std::make_shared<int>(0);
                    auto tick = std::make_shared<std::function<void()>>();
                    *tick = [&app, &ui, frames, tick] {
                        if (++*frames == 5)
                            app.undo();
                        if (*frames == 8) {
                            app.menuBar().openMenu(1);
                            return;
                        }
                        ui.post(*tick);
                    };
                    ui.post(*tick);
                }
                else if (demo == "browser") {
                    app.store().select(0);
                    app.chooseInstrument(); // the library's instruments, the track's selected
                }
            });
        }

        environment.run();
    } catch (const std::exception &e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }
    return 0;
}
