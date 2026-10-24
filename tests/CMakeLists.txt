# The self-tests, run by the program's --<suite>-selftest flags (tools/test.sh runs them all). A library
# linked into the program, so each suite checks the code exactly as the app runs it.
add_library(vybn_selftests STATIC
    AudioSelfTest.cpp
    BridgeSelfTest.cpp
    BrowserSelfTest.cpp
    EngineSelfTest.cpp
    ExpressionSelfTest.cpp
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
    Vst3SelfTest.cpp
)
target_include_directories(vybn_selftests PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(vybn_selftests PUBLIC vybn_daw PRIVATE vybn_stb vybn_miniaudio)
# The render test's golden images, and the plugin test's plugins (built below)
target_compile_definitions(vybn_selftests PRIVATE
    VYBN_TEST_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
    ENGINE_TEST_PLUGIN="${VYBN_OUTPUT_DIR}/plugins/vybn-test-plugins.clap"
    VST3_TEST_PLUGIN="${VYBN_OUTPUT_DIR}/plugins/vybn-test-plugins.vst3"
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

# The plugin self-test's VST3 plugins, as a bundle in output/plugins/: a gain with an editor, a sine
# instrument, a ducker with a sidechain bus and its own edit controller, and a twin of the CLAP gain
add_library(vybn-test-plugins-vst3 MODULE plugins/TestVst3.cpp)
target_link_libraries(vybn-test-plugins-vst3 PRIVATE vybn_vst3 gdi32 user32)
set_target_properties(vybn-test-plugins-vst3 PROPERTIES PREFIX "" OUTPUT_NAME "vybn-test-plugins" SUFFIX ".vst3")
vybn_warnings(vybn-test-plugins-vst3)
vybn_standalone(vybn-test-plugins-vst3)
vybn_output(vybn-test-plugins-vst3 FOLDER plugins/vybn-test-plugins.vst3/Contents/x86_64-win)
# and the bundle's list of its classes, read by a scan instead of loading it
add_custom_command(TARGET vybn-test-plugins-vst3 POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "${VYBN_OUTPUT_DIR}/plugins/vybn-test-plugins.vst3/Contents/Resources"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/plugins/moduleinfo.json"
            "${VYBN_OUTPUT_DIR}/plugins/vybn-test-plugins.vst3/Contents/Resources/moduleinfo.json")
set_property(TARGET vybn-test-plugins-vst3 APPEND PROPERTY LINK_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/plugins/moduleinfo.json")
