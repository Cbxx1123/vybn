# Third-party code, fetched at pinned versions on the first configure (needs network access and git).
# Each is offered as a target to link: glfw (built), and header-only interface targets for the rest.

include(FetchContent)

# Vulkan: the SDK named by the VULKAN_SDK environment variable
find_package(Vulkan REQUIRED)

# GLFW 3.4: windows and input, built from source as a static library
set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
FetchContent_Declare(glfw GIT_REPOSITORY https://github.com/glfw/glfw.git GIT_TAG 3.4 GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(glfw)

# stb: stb_truetype (compiled in src/ui/Font.cpp) and stb_image's inflate (src/daw/library/Archive.cpp).
# A system include, so its own warnings stay quiet.
FetchContent_Declare(stb GIT_REPOSITORY https://github.com/nothings/stb.git GIT_TAG 2c980bb59875b0d32144a71867fbdebb2f77cd20)
FetchContent_MakeAvailable(stb)
add_library(vybn_stb INTERFACE)
target_include_directories(vybn_stb SYSTEM INTERFACE ${stb_SOURCE_DIR})

# miniaudio 0.11.25: audio devices (WASAPI on Windows) and decoding. Only populated: its own CMake
# project builds libraries and examples not needed here; the implementation is compiled once, in
# src/audio/Miniaudio.cpp. Its switches are interface definitions, so every file that includes it
# agrees: no high-level engine, node graph or resource manager (the AudioSystem has its own graph),
# and WASAPI alone on Windows.
FetchContent_Declare(miniaudio GIT_REPOSITORY https://github.com/mackron/miniaudio.git GIT_TAG 9634bedb5b5a2ca38c1ee7108a9358a4e233f14d)
FetchContent_GetProperties(miniaudio)
if(NOT miniaudio_POPULATED)
    FetchContent_Populate(miniaudio)
endif()
add_library(vybn_miniaudio INTERFACE)
target_include_directories(vybn_miniaudio SYSTEM INTERFACE ${miniaudio_SOURCE_DIR})
target_compile_definitions(vybn_miniaudio INTERFACE MA_NO_ENGINE MA_NO_NODE_GRAPH MA_NO_RESOURCE_MANAGER)
if(WIN32)
    target_compile_definitions(vybn_miniaudio INTERFACE MA_ENABLE_ONLY_SPECIFIC_BACKENDS MA_ENABLE_WASAPI)
endif()

# CLAP 1.2.9: the plugin standard's C headers, for the app, the plugin host and the test plugins
FetchContent_Declare(clap GIT_REPOSITORY https://github.com/free-audio/clap.git GIT_TAG cd94482ba5941ae410809b6fbaed3bc851044270)
FetchContent_GetProperties(clap)
if(NOT clap_POPULATED)
    FetchContent_Populate(clap)
endif()
add_library(vybn_clap INTERFACE)
target_include_directories(vybn_clap SYSTEM INTERFACE ${clap_SOURCE_DIR}/include)

# VST3 3.8.0 (MIT): Steinberg's interface headers only, for the plugin host and the VST3 test plugins.
# They include each other as "pluginterfaces/...", so they are fetched into a folder of that name.
FetchContent_Declare(vst3_pluginterfaces GIT_REPOSITORY https://github.com/steinbergmedia/vst3_pluginterfaces.git
    GIT_TAG 31d6eeba6daaa3e2a8bfbe3e7a90ca0b7fbfbc1c # v3.8.0_build_66
    SOURCE_DIR ${FETCHCONTENT_BASE_DIR}/vst3/pluginterfaces)
FetchContent_GetProperties(vst3_pluginterfaces)
if(NOT vst3_pluginterfaces_POPULATED)
    FetchContent_Populate(vst3_pluginterfaces)
endif()
add_library(vybn_vst3 INTERFACE)
target_include_directories(vybn_vst3 SYSTEM INTERFACE ${FETCHCONTENT_BASE_DIR}/vst3)
