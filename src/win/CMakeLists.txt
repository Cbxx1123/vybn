# The icon patcher: gives the executable the icon of the user's theme once the app has ended (see
# IconPatch.cpp and daw/app/AppIcon.cpp). A plain Windows program with no console.
add_executable(vibin-icon-patch WIN32 IconPatch.cpp)
target_link_libraries(vibin-icon-patch PRIVATE shell32)
vibin_warnings(vibin-icon-patch)
vibin_standalone(vibin-icon-patch)
vibin_output(vibin-icon-patch)
