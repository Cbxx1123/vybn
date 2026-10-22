vibin_module(vibin_audio SOURCES
    AudioFile.cpp
    AudioSystem.cpp
    Automation.cpp
    ClipPlayer.cpp
    Device.cpp
    DeviceChain.cpp
    Effects.cpp
    Encoders.cpp
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
    Synth.cpp
    SynthPresets.cpp
    SynthTables.cpp
    Track.cpp
)
target_link_libraries(vibin_audio PUBLIC vibin_env vibin_plugin PRIVATE vibin_miniaudio)
if(WIN32)
    # MIDI input (midiIn), and MP3 export through Media Foundation (its GUIDs; its DLLs load when an
    # MP3 is written) and COM
    target_link_libraries(vibin_audio PRIVATE winmm mfuuid ole32)
endif()

# The real-time code (what the audio thread and the export engine run every sample) is compiled
# optimised in every build type (-O3: GCC 10 vectorises only there), keeping its debug information:
# the app is run as a Debug build, and at -O0 this code took two to three times as long.
# -DVIBIN_OPTIMISE_AUDIO=OFF to step through it in the debugger.
if(VIBIN_OPTIMISE_AUDIO AND NOT MSVC)
    set_source_files_properties(
        AudioSystem.cpp Automation.cpp ClipPlayer.cpp DeviceChain.cpp Effects.cpp Encoders.cpp Mastering.cpp
        Memory.cpp NotePlayer.cpp PluginEffect.cpp PluginInstrument.cpp Preview.cpp Processors.cpp Recorder.cpp
        Sampler.cpp Synth.cpp SynthTables.cpp Track.cpp
        PROPERTIES COMPILE_OPTIONS "-O3")
endif()
