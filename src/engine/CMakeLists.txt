vybn_module(vybn_engine SOURCES
    Buffer.cpp
    Descriptors.cpp
    FileDialog.cpp
    Http.cpp
    InputEvents.cpp
    Pipeline.cpp
    Renderer.cpp
    Shader.cpp
    Shell.cpp
    Texture.cpp
    Vulkan.cpp
    Window.cpp
    WindowSystem.cpp
)
target_link_libraries(vybn_engine PUBLIC vybn_env Vulkan::Vulkan glfw)
# Where the shaders and the assets (the font, the icon) are read from at run time
target_compile_definitions(vybn_engine PUBLIC
    ENGINE_SHADER_DIR="${SHADER_DIR}"
    ENGINE_ASSET_DIR="${PROJECT_SOURCE_DIR}/assets"
)
if(WIN32)
    # DWM (the window keeps its shadow under the app's own title bar), Windows' file dialogs (the
    # fallback), HTTPS through the system (downloads) and opening pages and folders
    target_link_libraries(vybn_engine PRIVATE dwmapi comdlg32 winhttp shell32)
endif()
