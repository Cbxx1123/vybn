# The shaders: GLSL in shaders/ compiled to SPIR-V in output/shaders/ by glslc from the Vulkan SDK,
# as the `shaders` target. Today only prim.vert and prim.frag, which draw every kind of UI primitive.

find_program(GLSLC glslc HINTS "$ENV{VULKAN_SDK}/Bin")
if(NOT GLSLC)
    message(FATAL_ERROR "glslc not found; check that VULKAN_SDK is set")
endif()

set(SHADER_DIR "${VIBIN_OUTPUT_DIR}/shaders")
set(SHADER_SOURCES
    shaders/prim.vert
    shaders/prim.frag
)

set(SHADER_BINARIES)
foreach(source IN LISTS SHADER_SOURCES)
    get_filename_component(name ${source} NAME)
    set(binary "${SHADER_DIR}/${name}.spv")
    add_custom_command(
        OUTPUT ${binary}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${SHADER_DIR}
        COMMAND ${GLSLC} ${PROJECT_SOURCE_DIR}/${source} -o ${binary}
        DEPENDS ${PROJECT_SOURCE_DIR}/${source}
        COMMENT "Compiling shader ${name}"
    )
    list(APPEND SHADER_BINARIES ${binary})
endforeach()
add_custom_target(shaders DEPENDS ${SHADER_BINARIES})
