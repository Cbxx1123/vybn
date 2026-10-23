cmake_minimum_required(VERSION 3.16)

project(VYBN LANGUAGES CXX)

if(MSVC)
    enable_language(RC) # the executable's icon (cmake/Windows.cmake)
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# A Debug build unless asked otherwise: the app is run and debugged as one (the audio engine is
# optimised in every build type all the same: see src/audio/CMakeLists.txt)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

option(VYBN_OPTIMISE_AUDIO "Compile the real-time audio code with -O3 in every build type" ON)
# The modules instrumented for gcov, to see which lines the self-tests run (tools/coverage.sh, which
# builds them in a tree and an output folder of their own)
option(VYBN_COVERAGE "Build the program's modules with gcov's instrumentation" OFF)

# Everything built goes here: the program, its helpers, the shaders and the test plugins
set(VYBN_OUTPUT_DIR "${PROJECT_SOURCE_DIR}/output" CACHE PATH "Where everything built goes")

list(APPEND CMAKE_MODULE_PATH "${PROJECT_SOURCE_DIR}/cmake")
include(Dependencies)
include(Shaders)
include(Targets)
include(Windows)

add_subdirectory(src)
add_subdirectory(tests)
