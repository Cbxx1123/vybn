vybn_module(vybn_plugin SOURCES
    Instance.cpp
    Manager.cpp
    ModuleInfo.cpp
    Sandbox.cpp
    Slot.cpp
)
target_link_libraries(vybn_plugin PUBLIC vybn_env vybn_clap)
if(WIN32)
    # The sandbox: AppContainers (userenv) and access lists (advapi32)
    target_link_libraries(vybn_plugin PRIVATE userenv advapi32)
endif()

# The plugin host: one sandboxed process per plugin instance (see Sandbox.h), CLAP or VST3. Small on
# purpose: Windows, the CLAP and VST3 headers and the shared protocol (Protocol.h), no Vulkan or UI
add_executable(vybn-plugin-host host/HostMain.cpp host/ClapFormat.cpp host/Vst3Format.cpp)
target_include_directories(vybn-plugin-host PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(vybn-plugin-host PRIVATE vybn_clap vybn_vst3 user32 gdi32)
vybn_warnings(vybn-plugin-host)
vybn_standalone(vybn-plugin-host)
vybn_output(vybn-plugin-host)
