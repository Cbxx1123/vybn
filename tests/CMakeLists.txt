# The self-tests, run by the program's --<suite>-selftest flags (tools/test.sh runs them all). A library
# linked into the program, so each suite checks the code exactly as the app runs it.
add_library(vybn_selftests STATIC
    AudioSelfTest.cpp
    BridgeSelfTest.cpp
    BrowserSelfTest.cpp
    EngineSelfTest.cpp
    ExportSelfTest.cpp
    InputSelfTest.cpp
    MarkersSelfTest.cpp
    MidiImportSelfTest.cpp
    NoteSelfTest.cpp
    PluginSelfTest.cpp
    ProjectSelfTest.cpp
    RenderSelfTest.cpp
    RoutingSelfTest.cpp
    StepSelfTest.cpp
    SynthSelfTest.cpp
    TakesSelfTest.cpp
    TempoSelfTest.cpp
)
target_include_directories(vybn_selftests PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(vybn_selftests PUBLIC vybn_daw PRIVATE vybn_stb vybn_miniaudio)
# The render test's golden images, and the plugin test's plugins (built below)
target_compile_definitions(vybn_selftests PRIVATE
    VYBN_TEST_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
    ENGINE_TEST_PLUGIN="${VYBN_OUTPUT_DIR}/plugins/vybn-test-plugins.clap"
)
vybn_warnings(vybn_selftests)

# The plugin self-test's CLAP plugins, in output/plugins/: a gain, a sine instrument, one that
# misbehaves on request, a ducker with a sidechain input and one that crashes when made
add_library(vybn-test-plugins MODULE plugins/TestPlugins.cpp)
target_link_libraries(vybn-test-plugins PRIVATE vybn_clap gdi32 user32)
set_target_properties(vybn-test-plugins PROPERTIES PREFIX "" SUFFIX ".clap")
vybn_warnings(vybn-test-plugins)
vybn_standalone(vybn-test-plugins)
vybn_output(vybn-test-plugins FOLDER plugins)
