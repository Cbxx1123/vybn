cmake_minimum_required(VERSION 3.16)

project(UIEngine LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Default to a Debug build so -g is set, matching the old Makefile
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

# Vulkan SDK: found through the VULKAN_SDK environment variable
find_package(Vulkan REQUIRED)

# GLFW: fetched and built from source at a pinned release
include(FetchContent)
set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    glfw
    GIT_REPOSITORY https://github.com/glfw/glfw.git
    GIT_TAG 3.4
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(glfw)

# stb (for stb_truetype): header-only, fetched at a pinned commit; there is nothing to build
FetchContent_Declare(
    stb
    GIT_REPOSITORY https://github.com/nothings/stb.git
    GIT_TAG 2c980bb59875b0d32144a71867fbdebb2f77cd20
)
FetchContent_MakeAvailable(stb)

# miniaudio: single-header audio I/O (WASAPI on Windows), fetched at a pinned release (0.11.25).
# Only the header is used; its implementation is compiled in src/audio/Miniaudio.cpp.
FetchContent_Declare(
    miniaudio
    GIT_REPOSITORY https://github.com/mackron/miniaudio.git
    GIT_TAG 9634bedb5b5a2ca38c1ee7108a9358a4e233f14d
)
FetchContent_GetProperties(miniaudio)
if(NOT miniaudio_POPULATED)
    # Populate only: miniaudio's own CMakeLists builds libraries and examples we don't need
    FetchContent_Populate(miniaudio)
endif()

# CLAP: the plugin standard's C headers (MIT), fetched at a pinned release (1.2.9). Headers only,
# so only populated. The app, the sandboxed plugin host and the test plugins include them.
FetchContent_Declare(
    clap
    GIT_REPOSITORY https://github.com/free-audio/clap.git
    GIT_TAG cd94482ba5941ae410809b6fbaed3bc851044270
)
FetchContent_GetProperties(clap)
if(NOT clap_POPULATED)
    FetchContent_Populate(clap)
endif()

# Shaders: GLSL in shaders/ is compiled to SPIR-V in output/shaders/ with glslc from the Vulkan SDK
find_program(GLSLC glslc HINTS "$ENV{VULKAN_SDK}/Bin")
if(NOT GLSLC)
    message(FATAL_ERROR "glslc not found; check that VULKAN_SDK is set")
endif()

set(SHADER_DIR "${CMAKE_SOURCE_DIR}/output/shaders")
set(SHADER_SOURCES
    shaders/ui.vert
    shaders/ui.frag
    shaders/text.vert
    shaders/text.frag
    shaders/image.frag
)
set(SHADER_BINARIES)
foreach(src IN LISTS SHADER_SOURCES)
    get_filename_component(name ${src} NAME)
    set(bin "${SHADER_DIR}/${name}.spv")
    add_custom_command(
        OUTPUT ${bin}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${SHADER_DIR}
        COMMAND ${GLSLC} ${CMAKE_SOURCE_DIR}/${src} -o ${bin}
        DEPENDS ${CMAKE_SOURCE_DIR}/${src}
        COMMENT "Compiling shader ${name}"
    )
    list(APPEND SHADER_BINARIES ${bin})
endforeach()
add_custom_target(shaders DEPENDS ${SHADER_BINARIES})

add_executable(main
    src/main.cpp
    src/env/Environment.cpp
    src/env/Scheduler.cpp
    src/env/Resources.cpp
    src/env/RealtimePool.cpp
    src/env/Semaphore.cpp
    src/engine/Window.cpp
    src/engine/Vulkan.cpp
    src/engine/Shader.cpp
    src/engine/Pipeline.cpp
    src/engine/Buffer.cpp
    src/engine/Texture.cpp
    src/engine/Descriptors.cpp
    src/engine/Renderer.cpp
    src/engine/WindowSystem.cpp
    src/engine/FileDialog.cpp
    src/engine/InputEvents.cpp
    src/engine/Http.cpp
    src/engine/Shell.cpp
    src/audio/Miniaudio.cpp
    src/audio/AudioSystem.cpp
    src/audio/Processors.cpp
    src/audio/Effects.cpp
    src/audio/Mastering.cpp
    src/audio/Encoders.cpp
    src/audio/Device.cpp
    src/audio/DeviceChain.cpp
    src/audio/PluginEffect.cpp
    src/audio/PluginInstrument.cpp
    src/audio/SampleSet.cpp
    src/audio/Sampler.cpp
    src/audio/Memory.cpp
    src/audio/NotePlayer.cpp
    src/audio/Track.cpp
    src/audio/AudioFile.cpp
    src/audio/ClipPlayer.cpp
    src/audio/Midi.cpp
    src/audio/Recorder.cpp
    src/audio/Preview.cpp
    src/audio/SynthTables.cpp
    src/audio/Synth.cpp
    src/audio/SynthPresets.cpp
    src/ui/Font.cpp
    src/ui/Theme.cpp
    src/ui/Layout.cpp
    src/ui/Components.cpp
    src/ui/Controls.cpp
    src/ui/Navigation.cpp
    src/ui/Menus.cpp
    src/ui/Dialogs.cpp
    src/ui/FileDialog.cpp
    src/ui/Docking.cpp
    src/ui/Panels.cpp
    src/ui/UISystem.cpp
    src/ui/UIRenderer.cpp
    src/ui/Context.cpp
    src/ui/Keymap.cpp
    src/ui/Signals.cpp
    # The DAW's UI, built from the UI layer
    src/daw/Project.cpp
    src/daw/Toml.cpp
    src/daw/ProjectFile.cpp
    src/daw/Actions.cpp
    src/daw/ShortcutsEditor.cpp
    src/daw/Settings.cpp
    src/daw/Library.cpp
    src/daw/DeviceCatalogue.cpp
    src/daw/Widgets.cpp
    src/daw/Arpeggiator.cpp
    src/daw/Arrangement.cpp
    src/daw/Mixer.cpp
    src/daw/DevicesPanel.cpp
    src/daw/DevicePicker.cpp
    src/daw/DeviceView.cpp
    src/daw/DeviceControls.cpp
    src/daw/SynthView.cpp
    src/daw/SynthUserPresets.cpp
    src/daw/Splash.cpp
    src/daw/PianoRoll.cpp
    src/daw/NoteTools.cpp
    src/daw/History.cpp
    src/daw/ExportModel.cpp
    src/daw/MidiExport.cpp
    src/daw/RenderJob.cpp
    src/daw/TransportBar.cpp
    src/daw/AudioBridge.cpp
    src/daw/Autosave.cpp
    src/daw/PreviewPlayer.cpp
    src/daw/PianoRollMenus.cpp
    src/daw/StepSequence.cpp
    src/daw/StepSequencer.cpp
    src/daw/StepMenus.cpp
    src/daw/ProjectSession.cpp
    src/daw/ProjectStore.cpp
    src/daw/DawApp.cpp
    src/daw/DawAudio.cpp
    src/daw/DawSaving.cpp
    src/daw/AudioPool.cpp
    src/daw/Recording.cpp
    src/daw/AudioEditor.cpp
    src/daw/Json.cpp
    src/daw/Archive.cpp
    src/daw/Online.cpp
    src/daw/Browser.cpp
    src/daw/DawBrowser.cpp
    src/daw/Exporter.cpp
    src/daw/ExportDialog.cpp
    src/daw/ExportsPanel.cpp
    src/daw/RecordingController.cpp
    src/daw/DawPlugins.cpp
    src/daw/PluginEditor.cpp
    src/daw/Appearance.cpp
    # Plugins: the app's side of the sandboxed CLAP hosts
    src/plugin/Sandbox.cpp
    src/plugin/Instance.cpp
    src/plugin/Manager.cpp
    src/plugin/Slot.cpp
    # Automated checks run from the command line (main --audio-selftest, --project-selftest,
    # --input-selftest, --notes-selftest, --engine-selftest, --browser-selftest, --plugin-selftest,
    # --synth-selftest, --export-selftest)
    tests/AudioSelfTest.cpp
    tests/InputSelfTest.cpp
    tests/ProjectSelfTest.cpp
    tests/BrowserSelfTest.cpp
    tests/NoteSelfTest.cpp
    tests/StepSelfTest.cpp
    tests/BridgeSelfTest.cpp
    tests/RenderSelfTest.cpp
    tests/EngineSelfTest.cpp
    tests/PluginSelfTest.cpp
    tests/SynthSelfTest.cpp
    tests/ExportSelfTest.cpp
)
add_dependencies(main shaders vibin-plugin-host vibin-test-plugins)

target_include_directories(main PRIVATE src tests)
# stb as a system include, so its own warnings don't show up under -Wall -Wextra
target_include_directories(main SYSTEM PRIVATE ${stb_SOURCE_DIR} ${miniaudio_SOURCE_DIR} ${clap_SOURCE_DIR}/include)
# miniaudio's configuration, the same for every file that includes it: no high-level engine,
# node graph or resource manager (the AudioSystem has its own graph), and WASAPI only on Windows
target_compile_definitions(main PRIVATE MA_NO_ENGINE MA_NO_NODE_GRAPH MA_NO_RESOURCE_MANAGER)
if(WIN32)
    target_compile_definitions(main PRIVATE MA_ENABLE_ONLY_SPECIFIC_BACKENDS MA_ENABLE_WASAPI)
endif()
target_link_libraries(main PRIVATE Vulkan::Vulkan glfw)
if(WIN32)
    # DWM keeps the window shadow when the app draws its own title bar
    target_link_libraries(main PRIVATE dwmapi gdiplus)
    # The open-file dialog for importing instruments
    target_link_libraries(main PRIVATE comdlg32)
    # MIDI input (midiIn*)
    target_link_libraries(main PRIVATE winmm)
    # Downloads for the browser's Online tab (HTTPS through the system), and opening pages and folders
    target_link_libraries(main PRIVATE winhttp shell32)
    # The plugin sandbox: AppContainers (userenv) and access lists (advapi32)
    target_link_libraries(main PRIVATE userenv advapi32)
endif()
target_compile_definitions(main PRIVATE
    ENGINE_SHADER_DIR="${SHADER_DIR}"
    ENGINE_ASSET_DIR="${CMAKE_SOURCE_DIR}/assets"
    VIBIN_TEST_DIR="${CMAKE_SOURCE_DIR}/tests"
    ENGINE_TEST_PLUGIN="${CMAKE_SOURCE_DIR}/output/plugins/vibin-test-plugins.clap"
)

if(MSVC)
    target_compile_options(main PRIVATE /W4)
else()
    target_compile_options(main PRIVATE -Wall -Wextra)
endif()

set_target_properties(main PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/output"
)

# The plugin host: one sandboxed process per plugin instance (see src/plugin/Sandbox.h). Small on
# purpose: Windows, the CLAP headers and the shared protocol, no Vulkan or UI. Linked statically
# so the sandbox needs to read nothing but the executable and system DLLs.
add_executable(vibin-plugin-host src/plugin/host/HostMain.cpp)
target_include_directories(vibin-plugin-host PRIVATE src)
target_link_libraries(vibin-plugin-host PRIVATE user32 gdi32)
target_include_directories(vibin-plugin-host SYSTEM PRIVATE ${clap_SOURCE_DIR}/include)
set_target_properties(vibin-plugin-host PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/output")

# The plugin self-test's CLAP plugins: a gain, a sine instrument and one that misbehaves
add_library(vibin-test-plugins MODULE tests/plugins/TestPlugins.cpp)
target_include_directories(vibin-test-plugins SYSTEM PRIVATE ${clap_SOURCE_DIR}/include)
target_link_libraries(vibin-test-plugins PRIVATE gdi32 user32)
set_target_properties(vibin-test-plugins PROPERTIES
    PREFIX ""
    SUFFIX ".clap"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/output/plugins"
)

foreach(target vibin-plugin-host vibin-test-plugins)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra)
        target_link_options(${target} PRIVATE -static -static-libgcc -static-libstdc++)
    endif()
endforeach()
