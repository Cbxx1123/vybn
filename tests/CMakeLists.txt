# The self-tests, run by the program's --<suite>-selftest flags (tools/test.sh runs them all). A library
# linked into the program, so each suite checks the code exactly as the app runs it.
add_library(vibin_selftests STATIC
    AudioSelfTest.cpp
    BridgeSelfTest.cpp
    BrowserSelfTest.cpp
    EngineSelfTest.cpp
    ExportSelfTest.cpp
    InputSelfTest.cpp
    NoteSelfTest.cpp
    PluginSelfTest.cpp
    ProjectSelfTest.cpp
    RenderSelfTest.cpp
    RoutingSelfTest.cpp
    StepSelfTest.cpp
    SynthSelfTest.cpp
)
target_include_directories(vibin_selftests PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(vibin_selftests PUBLIC vibin_daw PRIVATE vibin_stb vibin_miniaudio)
# The render test's golden images, and the plugin test's plugins (built below)
target_compile_definitions(vibin_selftests PRIVATE
    VIBIN_TEST_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
    ENGINE_TEST_PLUGIN="${VIBIN_OUTPUT_DIR}/plugins/vibin-test-plugins.clap"
)
vibin_warnings(vibin_selftests)

# The plugin self-test's CLAP plugins, in output/plugins/: a gain, a sine instrument, one that
# misbehaves on request, a ducker with a sidechain input and one that crashes when made
add_library(vibin-test-plugins MODULE plugins/TestPlugins.cpp)
target_link_libraries(vibin-test-plugins PRIVATE vibin_clap gdi32 user32)
set_target_properties(vibin-test-plugins PROPERTIES PREFIX "" SUFFIX ".clap")
vibin_warnings(vibin-test-plugins)
vibin_standalone(vibin-test-plugins)
vibin_output(vibin-test-plugins FOLDER plugins)
