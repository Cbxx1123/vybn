vybn_module(vybn_daw SOURCES
    # The project and its rules
    model/AutomationLanes.cpp
    model/ExportModel.cpp
    model/Freeze.cpp
    model/History.cpp
    model/NoteTools.cpp
    model/Project.cpp
    model/ProjectSession.cpp
    model/ProjectStore.cpp
    model/Routing.cpp
    model/StepSequence.cpp
    model/Takes.cpp
    model/Transcription.cpp
    model/Warp.cpp
    # Files and formats
    io/Autosave.cpp
    io/Json.cpp
    io/MidiExport.cpp
    io/MidiImport.cpp
    io/ProjectFile.cpp
    io/SynthUserPresets.cpp
    io/Toml.cpp
    # Instruments, downloads and the devices to choose from
    library/Archive.cpp
    library/DeviceCatalogue.cpp
    library/Library.cpp
    library/Online.cpp
    library/PreviewPlayer.cpp
    # The project played, recorded and rendered by the audio engine
    audio/AudioBridge.cpp
    audio/AudioPool.cpp
    audio/AutomationWriter.cpp
    audio/Exporter.cpp
    audio/Freezer.cpp
    audio/Recording.cpp
    audio/RecordingController.cpp
    audio/RenderJob.cpp
    # Panels, editors and dialogs
    views/Analyzer.cpp
    views/Arpeggiator.cpp
    views/Arrangement.cpp
    views/ArrangementTakes.cpp
    views/AudioEditor.cpp
    views/Browser.cpp
    views/DeviceControls.cpp
    views/DevicePicker.cpp
    views/DeviceView.cpp
    views/DevicesPanel.cpp
    views/ExportDialog.cpp
    views/ExportsPanel.cpp
    views/Mixer.cpp
    views/PianoRoll.cpp
    views/PianoRollMenus.cpp
    views/PluginEditor.cpp
    views/ShortcutsEditor.cpp
    views/StepMenus.cpp
    views/StepSequencer.cpp
    views/SynthView.cpp
    views/TimingLanes.cpp
    views/TransportBar.cpp
    # Controls and text formats the views share
    widgets/Widgets.cpp
    # The app: its window, actions, settings, the loading screen and the look
    app/Actions.cpp
    app/AppIcon.cpp
    app/Appearance.cpp
    app/DawApp.cpp
    app/DawAudio.cpp
    app/DawBrowser.cpp
    app/DawPlugins.cpp
    app/DawSaving.cpp
    app/DawWarp.cpp
    app/Settings.cpp
    app/Splash.cpp
)
target_link_libraries(vybn_daw PUBLIC vybn_ui vybn_audio vybn_plugin PRIVATE vybn_stb)
if(WIN32)
    # The loading screen (GDI+), the title bar (DWM), MIDI input, and the icon and folders (the shell)
    target_link_libraries(vybn_daw PRIVATE gdiplus dwmapi winmm shell32)
endif()
