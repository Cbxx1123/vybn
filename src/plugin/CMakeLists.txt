vibin_module(vibin_plugin SOURCES
    Instance.cpp
    Manager.cpp
    Sandbox.cpp
    Slot.cpp
)
target_link_libraries(vibin_plugin PUBLIC vibin_env vibin_clap)
if(WIN32)
    # The sandbox: AppContainers (userenv) and access lists (advapi32)
    target_link_libraries(vibin_plugin PRIVATE userenv advapi32)
endif()

# The plugin host: one sandboxed process per plugin instance (see Sandbox.h). Small on purpose:
# Windows, the CLAP headers and the shared protocol (Protocol.h), no Vulkan or UI
add_executable(vibin-plugin-host host/HostMain.cpp)
target_include_directories(vibin-plugin-host PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(vibin-plugin-host PRIVATE vibin_clap user32 gdi32)
vibin_warnings(vibin-plugin-host)
vibin_standalone(vibin-plugin-host)
vibin_output(vibin-plugin-host)
