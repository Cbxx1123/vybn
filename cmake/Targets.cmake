# Helpers the CMakeLists under src/ and tests/ share.

# A module of the program: a static library of `sources` (relative to the calling folder), reached by
# includes rooted at src/ ("audio/Track.h"), with the project's warnings
function(vybn_module name)
    cmake_parse_arguments(ARG "" "" "SOURCES" ${ARGN})
    add_library(${name} STATIC ${ARG_SOURCES})
    target_include_directories(${name} PUBLIC ${PROJECT_SOURCE_DIR}/src)
    vybn_warnings(${name})
    if(VYBN_COVERAGE)
        target_compile_options(${name} PRIVATE --coverage)
    endif()
endfunction()

# The project's warnings (-Wall -Wextra; warnings, not errors)
function(vybn_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra)
    endif()
endfunction()

# A helper program that must run on its own (the plugin host in its sandbox, the icon patcher after
# the app has gone): linked statically, so it needs nothing but itself and the system's DLLs
function(vybn_standalone target)
    if(NOT MSVC)
        target_link_options(${target} PRIVATE -static -static-libgcc -static-libstdc++)
    endif()
endfunction()

# Put a target's output in output/ (or a folder under it)
function(vybn_output target)
    cmake_parse_arguments(ARG "" "FOLDER" "" ${ARGN})
    set(dir "${VYBN_OUTPUT_DIR}")
    if(ARG_FOLDER)
        set(dir "${dir}/${ARG_FOLDER}")
    endif()
    set_target_properties(${target} PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${dir}" LIBRARY_OUTPUT_DIRECTORY "${dir}")
endfunction()
