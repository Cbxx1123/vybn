vybn_module(vybn_ui SOURCES
    Components.cpp
    Context.cpp
    Controls.cpp
    Dialogs.cpp
    Docking.cpp
    DrawCache.cpp
    DrawList.cpp
    FileDialog.cpp
    Font.cpp
    Keymap.cpp
    Layout.cpp
    Menus.cpp
    Navigation.cpp
    Panels.cpp
    Prim.cpp
    Signals.cpp
    Theme.cpp
    UIRenderer.cpp
    UISystem.cpp
)
target_link_libraries(vybn_ui PUBLIC vybn_engine PRIVATE vybn_stb)
