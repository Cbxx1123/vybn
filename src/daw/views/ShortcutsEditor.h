#pragma once

#include <functional>

#include "ui/UI.h"

namespace daw {

// The keyboard shortcuts dialog: every action in the keymap by category, with its keys. Click a
// key to change it, x to remove it, + to add another, Reset to go back to the defaults (one
// action, or all of them). Changing waits for the next key press, whatever it is (Escape
// cancels); a chord another action had is taken from it, and the dialog says which. A search
// field filters by name, category or keys. `onChange` runs after every change (to save them).
void openShortcutsEditor(ui::Context &context, ui::Keymap &keymap, std::function<void()> onChange);

}
