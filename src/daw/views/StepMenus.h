#pragma once

#include <vector>

#include "daw/views/StepSequencer.h"
#include "ui/UI.h"

namespace daw {

// The step sequencer's menus and its one dialog, for DawApp, built from the view's state each time
// they open. Every action goes through the view's row tools, so it is one undoable clip edit.

// A row's menu (right-click its name or a step): Select row, then Fill every 1, 2, 3,
// 4 and 8 steps (a sub-menu), Euclidean... (the dialog), Random (sparse 25%, medium 50%, dense
// 75%, a sub-menu, a new seed each time), Shift left and Shift right (a step), Copy row, Paste row
// (only when there is one), Mute row (a toggle), Clear row, and Remove row
std::vector<ui::MenuItem> stepRowMenu(ui::Context &context, StepSequencerView &view, int key);
// The rows to add: the GM percussion keys 35-81 by drumName in groups (Kicks and snares, Hats and
// cymbals, Toms, Percussion), rows already shown marked with a check
std::vector<ui::MenuItem> stepAddRowMenu(StepSequencerView &view);
// The step sizes (stepChoices by name, the current one checked)
std::vector<ui::MenuItem> stepSizeMenu(StepSequencerView &view);
// The Euclidean rhythm dialog for a row: Pulses (0 to the grid's count) and Rotation (0 to
// count - 1) as Steppers, a preview of the pattern drawn as a row of dots, Cancel and Apply
// (euclidRow). A modal ui::Dialog.
void openEuclidDialog(ui::Context &context, StepSequencerView &view, int key);

}
