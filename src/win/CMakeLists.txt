# The icon patcher: gives the executable the icon of the user's theme once the app has ended (see
# IconPatch.cpp and daw/app/AppIcon.cpp). A plain Windows program with no console.
add_executable(vybn-icon-patch WIN32 IconPatch.cpp)
target_link_libraries(vybn-icon-patch PRIVATE shell32)
vybn_warnings(vybn-icon-patch)
vybn_standalone(vybn-icon-patch)
vybn_output(vybn-icon-patch)
