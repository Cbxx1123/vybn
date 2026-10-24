vybn_module(vybn_audio SOURCES
    Analysis.cpp
    AudioAnalysis.cpp
    AudioFile.cpp
    AudioSystem.cpp
    Automation.cpp
    ClipPlayer.cpp
    Device.cpp
    DeviceChain.cpp
    Effects.cpp
    Encoders.cpp
    Fft.cpp
    Mastering.cpp
    Memory.cpp
    Midi.cpp
    Miniaudio.cpp
    Mp3Encoder.cpp
    NotePlayer.cpp
    PluginEffect.cpp
    PluginInstrument.cpp
    Preview.cpp
    Processors.cpp
    Recorder.cpp
    Routing.cpp
    SampleSet.cpp
    Sampler.cpp
    Stretch.cpp
    Synth.cpp
    SynthPresets.cpp
    SynthTables.cpp
    TempoMap.cpp
    Track.cpp
    Warp.cpp
)
target_link_libraries(vybn_audio PUBLIC vybn_env vybn_plugin PRIVATE vybn_miniaudio)
if(WIN32)
    # MIDI input (midiIn), and MP3 export through Media Foundation (its GUIDs; its DLLs load when an
    # MP3 is written) and COM
    target_link_libraries(vybn_audio PRIVATE winmm mfuuid ole32)
endif()

# The real-time code (what the audio thread and the export engine run every sample) is compiled
# optimised in every build type (-O3: GCC 10 vectorises only there), keeping its debug information:
# the app is run as a Debug build, and at -O0 this code took two to three times as long.
# -DVYBN_OPTIMISE_AUDIO=OFF to step through it in the debugger.
if(VYBN_OPTIMISE_AUDIO AND NOT MSVC)
    set_source_files_properties(
        AudioAnalysis.cpp AudioSystem.cpp Automation.cpp ClipPlayer.cpp DeviceChain.cpp Effects.cpp Encoders.cpp Fft.cpp Mastering.cpp
        Memory.cpp NotePlayer.cpp PluginEffect.cpp PluginInstrument.cpp Preview.cpp Processors.cpp Recorder.cpp
        Sampler.cpp Stretch.cpp Synth.cpp SynthTables.cpp TempoMap.cpp Track.cpp Warp.cpp Analysis.cpp
        PROPERTIES COMPILE_OPTIONS "-O3")
endif()
