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
add_executable(main main.cpp)
target_link_libraries(main PRIVATE vibin_daw vibin_selftests)
vibin_warnings(main)
vibin_output(main)
add_dependencies(main shaders vibin-plugin-host vibin-test-plugins)
if(WIN32)
    vibin_add_icon(main)
    add_dependencies(main vibin-icon-patch)
endif()
