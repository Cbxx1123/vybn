#pragma once

namespace tests {

// Offline checks of the piano roll's note tools (NoteTools): scales, chords, scale runs, the
// arpeggiator, quantize, legato, velocity shaping, duplicate, reverse, invert, chop, join, fixing
// overlaps, copy and paste, and keeping notes inside a clip. Prints PASS/FAIL per check; returns
// the process exit code.
int runNoteSelfTest();

}
