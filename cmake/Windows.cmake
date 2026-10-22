# The executable's icon, as a Windows resource: assets/icon/vybn.ico, the app's mark in the default
# look (made by `output/vybn --write-icon assets/icon/vybn.ico`; keep it in step with
# src/daw/app/AppIcon.cpp). A change to the icon rebuilds the resource.
function(vybn_add_icon target)
    set(VYBN_ICON_FILE "${PROJECT_SOURCE_DIR}/assets/icon/vybn.ico")
    configure_file("${PROJECT_SOURCE_DIR}/src/win/vybn.rc.in" "${CMAKE_BINARY_DIR}/vybn.rc" @ONLY)
    if(MSVC)
        target_sources(${target} PRIVATE "${CMAKE_BINARY_DIR}/vybn.rc")
        set_source_files_properties("${CMAKE_BINARY_DIR}/vybn.rc" PROPERTIES OBJECT_DEPENDS "${VYBN_ICON_FILE}")
        return()
    endif()
    # Compiled with the windres beside the compiler in use, by hand rather than as CMake's RC language:
    # the default MinGW (C:/MinGW, 32-bit) is first on PATH here, and its windres makes 32-bit objects,
    # which a 64-bit link refuses (and CMake's own detection looped re-configuring)
    get_filename_component(compiler_dir "${CMAKE_CXX_COMPILER}" DIRECTORY)
    find_program(VYBN_WINDRES NAMES windres HINTS "${compiler_dir}" NO_DEFAULT_PATH)
    if(NOT VYBN_WINDRES)
        message(WARNING "windres not found next to ${CMAKE_CXX_COMPILER}: the executable has no icon of its own")
        return()
    endif()
    set(object "${CMAKE_BINARY_DIR}/vybn.rc.o")
    set(format "")
    if(CMAKE_SIZEOF_VOID_P EQUAL 8)
        set(format -F pe-x86-64)
    endif()
    add_custom_command(
        OUTPUT "${object}"
        COMMAND "${VYBN_WINDRES}" ${format} -i "${CMAKE_BINARY_DIR}/vybn.rc" -o "${object}"
        DEPENDS "${CMAKE_BINARY_DIR}/vybn.rc" "${VYBN_ICON_FILE}"
        COMMENT "Compiling the executable's icon"
        VERBATIM)
    target_sources(${target} PRIVATE "${object}")
    set_source_files_properties("${object}" PROPERTIES EXTERNAL_OBJECT TRUE GENERATED TRUE)
endfunction()
