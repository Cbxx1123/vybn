#pragma once

namespace tests {

// Checks takes and comping (daw/model/Takes.h): slicing clips, comping ranges of audio and MIDI takes,
// boundaries, stepping, folding a recording into takes, flattening, the file round trip, ids and the
// undo labels. Prints PASS/FAIL per check; returns the failure count.
int runTakesChecks();

}
