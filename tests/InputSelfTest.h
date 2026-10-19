#pragma once

namespace tests {

// Offline checks of the input system, with no window: signal priority and consumption, hooks
// coming and going during an emit, chord names, the keymap (rebinding, conflicts, overrides,
// the keybinds file), key-to-action dispatch, and a text field driven by key and text events
// (typing, selection, word moves, select all, copy, cut, paste, and keys it lets through).
// Prints PASS/FAIL per check; returns the process exit code.
int runInputSelfTest();

}
