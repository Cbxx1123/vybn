cmake_minimum_required(VERSION 3.16)

project(UIEngine LANGUAGES CXX)

# Windows resources: the executable's icon (src/win/vibin.rc.in), compiled below
if(MSVC)
    enable_language(RC)
endif()

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
    shaders/prim.vert
    shaders/prim.frag
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
    src/audio/Automation.cpp
    src/audio/Routing.cpp
    src/audio/Encoders.cpp
    src/audio/Mp3Encoder.cpp
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
    src/ui/DrawList.cpp
    src/ui/DrawCache.cpp
    src/ui/Prim.cpp
    src/ui/Context.cpp
    src/ui/Keymap.cpp
    src/ui/Signals.cpp
    # The DAW's UI, built from the UI layer
    src/daw/model/Project.cpp
    src/daw/io/Toml.cpp
    src/daw/io/ProjectFile.cpp
    src/daw/app/Actions.cpp
    src/daw/views/ShortcutsEditor.cpp
    src/daw/app/Settings.cpp
    src/daw/library/Library.cpp
    src/daw/library/DeviceCatalogue.cpp
    src/daw/widgets/Widgets.cpp
    src/daw/views/Arpeggiator.cpp
    src/daw/views/Arrangement.cpp
    src/daw/views/Mixer.cpp
    src/daw/views/DevicesPanel.cpp
    src/daw/views/DevicePicker.cpp
    src/daw/views/DeviceView.cpp
    src/daw/views/DeviceControls.cpp
    src/daw/views/SynthView.cpp
    src/daw/io/SynthUserPresets.cpp
    src/daw/app/AppIcon.cpp
    src/daw/app/Splash.cpp
    src/daw/views/PianoRoll.cpp
    src/daw/model/NoteTools.cpp
    src/daw/model/History.cpp
    src/daw/model/ExportModel.cpp
    src/daw/io/MidiExport.cpp
    src/daw/audio/RenderJob.cpp
    src/daw/views/TransportBar.cpp
    src/daw/audio/AudioBridge.cpp
    src/daw/model/AutomationLanes.cpp
    src/daw/model/Routing.cpp
    src/daw/audio/AutomationWriter.cpp
    src/daw/io/Autosave.cpp
    src/daw/library/PreviewPlayer.cpp
    src/daw/views/PianoRollMenus.cpp
    src/daw/model/StepSequence.cpp
    src/daw/views/StepSequencer.cpp
    src/daw/views/StepMenus.cpp
    src/daw/model/ProjectSession.cpp
    src/daw/model/ProjectStore.cpp
    src/daw/app/DawApp.cpp
    src/daw/app/DawAudio.cpp
    src/daw/app/DawSaving.cpp
    src/daw/audio/AudioPool.cpp
    src/daw/audio/Recording.cpp
    src/daw/views/AudioEditor.cpp
    src/daw/io/Json.cpp
    src/daw/library/Archive.cpp
    src/daw/library/Online.cpp
    src/daw/views/Browser.cpp
    src/daw/app/DawBrowser.cpp
    src/daw/audio/Exporter.cpp
    src/daw/views/ExportDialog.cpp
    src/daw/views/ExportsPanel.cpp
    src/daw/audio/RecordingController.cpp
    src/daw/app/DawPlugins.cpp
    src/daw/views/PluginEditor.cpp
    src/daw/app/Appearance.cpp
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
    tests/RoutingSelfTest.cpp
    tests/RenderSelfTest.cpp
    tests/EngineSelfTest.cpp
    tests/PluginSelfTest.cpp
    tests/SynthSelfTest.cpp
    tests/ExportSelfTest.cpp
)
add_dependencies(main shaders vibin-plugin-host vibin-test-plugins)

# vibin-icon-patch: gives the executable the icon of the user's theme once the app has ended (see
# src/win/IconPatch.cpp and AppIcon.cpp). A plain Windows program with no console, linked statically.
if(WIN32)
    add_executable(vibin-icon-patch WIN32 src/win/IconPatch.cpp)
    target_link_libraries(vibin-icon-patch PRIVATE shell32)
    if(NOT MSVC)
        target_link_options(vibin-icon-patch PRIVATE -static -static-libgcc -static-libstdc++)
    endif()
    set_target_properties(vibin-icon-patch PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/output")
    add_dependencies(main vibin-icon-patch)
endif()

if(WIN32)
    # The executable's icon: the app's mark in the default look. Made by `output/main --write-icon
    # assets/icon/vibin.ico` (keep it in step with src/daw/app/AppIcon.cpp); a change to it rebuilds the resource.
    set(VIBIN_ICON_FILE "${CMAKE_SOURCE_DIR}/assets/icon/vibin.ico")
    configure_file(src/win/vibin.rc.in "${CMAKE_BINARY_DIR}/vibin.rc" @ONLY)
    if(MSVC)
        target_sources(main PRIVATE "${CMAKE_BINARY_DIR}/vibin.rc")
        set_source_files_properties("${CMAKE_BINARY_DIR}/vibin.rc" PROPERTIES OBJECT_DEPENDS "${VIBIN_ICON_FILE}")
    else()
        # Compiled with the windres that comes with the compiler in use, by hand rather than as CMake's RC
        # language: the default MinGW (C:/MinGW, 32-bit) is first on PATH here, and its windres makes
        # 32-bit objects, which a 64-bit link refuses
        get_filename_component(_vibin_compiler_dir "${CMAKE_CXX_COMPILER}" DIRECTORY)
        find_program(VIBIN_WINDRES NAMES windres HINTS "${_vibin_compiler_dir}" NO_DEFAULT_PATH)
        if(VIBIN_WINDRES)
            set(_vibin_rc_object "${CMAKE_BINARY_DIR}/vibin.rc.o")
            set(_vibin_rc_format "")
            if(CMAKE_SIZEOF_VOID_P EQUAL 8)
                set(_vibin_rc_format -F pe-x86-64)
            endif()
            add_custom_command(
                OUTPUT "${_vibin_rc_object}"
                COMMAND "${VIBIN_WINDRES}" ${_vibin_rc_format} -i "${CMAKE_BINARY_DIR}/vibin.rc" -o "${_vibin_rc_object}"
                DEPENDS "${CMAKE_BINARY_DIR}/vibin.rc" "${VIBIN_ICON_FILE}"
                COMMENT "Compiling the executable's icon"
                VERBATIM)
            target_sources(main PRIVATE "${_vibin_rc_object}")
            set_source_files_properties("${_vibin_rc_object}" PROPERTIES EXTERNAL_OBJECT TRUE GENERATED TRUE)
        else()
            message(WARNING "windres not found next to ${CMAKE_CXX_COMPILER}: the executable has no icon of its own")
        endif()
    endif()
endif()

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
    # MP3 export: Media Foundation's GUIDs (its DLLs are loaded when an MP3 is written) and COM
    target_link_libraries(main PRIVATE mfuuid ole32)
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

# The audio engine's real-time code (what the audio thread and the export engine run every sample) is
# compiled optimised in every build type (-O3: GCC 10 vectorises only there), keeping its debug
# information: the app is run as a Debug build, and at -O0 this code took two to three times as long.
# OFF to step through it in the debugger.
option(VIBIN_OPTIMISE_AUDIO "Compile the real-time audio code with -O3 in every build type" ON)
if(VIBIN_OPTIMISE_AUDIO AND NOT MSVC)
    set_source_files_properties(
        src/audio/AudioSystem.cpp src/audio/Automation.cpp src/audio/ClipPlayer.cpp src/audio/DeviceChain.cpp
        src/audio/Effects.cpp src/audio/Encoders.cpp src/audio/Mastering.cpp src/audio/Memory.cpp
        src/audio/NotePlayer.cpp src/audio/PluginEffect.cpp src/audio/PluginInstrument.cpp src/audio/Preview.cpp
        src/audio/Processors.cpp src/audio/Recorder.cpp src/audio/Sampler.cpp src/audio/Synth.cpp
        src/audio/SynthTables.cpp src/audio/Track.cpp
        PROPERTIES COMPILE_OPTIONS "-O3")
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
