vibin_module(vibin_ui SOURCES
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
target_link_libraries(vibin_ui PUBLIC vibin_engine PRIVATE vibin_stb)
