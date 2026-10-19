#pragma once

#include <functional>
#include <string>

#include "ProjectStore.h"
#include "ui/UI.h"

namespace daw {

// What the export dialog reaches through the app: the render of a preset, by its index in the
// project's presets (queued, with the Exports panel shown)
struct ExportDialogHooks {
    std::function<void(size_t preset)> run;
};

// Project > Export (Ctrl+Shift+E): a large dialog over the window with three columns, the project's
// export presets (add, copy, rename, delete, reset to the defaults), the selected preset's targets
// (enable, add, copy, delete) and the selected target's settings, with the export details below.
// Every control reads the store each frame and writes through it only when the value really
// changes, so each change is an undoable edit. Export renders the selected preset and closes it.
void openExportDialog(ui::Context &context, ProjectStore &store, ExportDialogHooks hooks);

}
