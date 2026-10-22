# The program's modules, each a static library, in the order they build on one another:
#   env      the environment: daemons, the scheduler and its thread pools, resources
#   engine   windows, input and Vulkan (GLFW)
#   ui       the UI layer: layout, components, drawing, the UI system
#   plugin   CLAP plugins, each hosted in a sandboxed process (and that host, plugin/host)
#   audio    the audio system, devices, instruments, effects, routing, encoders
#   daw      the DAW itself: model, files, library, the audio bridge, views and the app
#   win      Windows-only helpers (the icon patcher)
add_subdirectory(env)
add_subdirectory(engine)
add_subdirectory(ui)
add_subdirectory(plugin)
add_subdirectory(audio)
add_subdirectory(daw)
if(WIN32)
    add_subdirectory(win)
endif()

# The program: main.cpp reads the flags, builds the environment and runs the app, or a self-test
add_executable(vybn main.cpp)
target_link_libraries(vybn PRIVATE vybn_daw vybn_selftests)
vybn_warnings(vybn)
vybn_output(vybn)
add_dependencies(vybn shaders vybn-plugin-host vybn-test-plugins)
if(WIN32)
    vybn_add_icon(vybn)
    add_dependencies(vybn vybn-icon-patch)
endif()
