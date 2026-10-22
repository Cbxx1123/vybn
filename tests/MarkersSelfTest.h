#pragma once

namespace tests {

// Checks markers (Project::markers): finding them around a beat, the file round trip, ids, the undo
// labels and an undo through the store. Prints PASS/FAIL per check; returns the failure count.
int runMarkersChecks();

}
