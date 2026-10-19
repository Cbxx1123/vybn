#pragma once

namespace tests {

// Checks that AudioBridge keeps each track's processor across structural edits (delete, undo, reorder,
// kind change, added tracks, dropAll) with a deviceless engine. Prints PASS/FAIL per check; returns the failure count.
int runBridgeChecks();

}
