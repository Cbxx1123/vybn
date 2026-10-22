#pragma once

namespace tests {

// Checks MIDI import (daw/io/MidiImport.h): a file the exporter wrote read back, hand-made files in the
// forms the standard allows (running status, velocity-0 note offs, a type 0 file's channels),
// refusals, putting parts onto tracks, and the file's tempo map laid over the project's. Prints
// PASS/FAIL per check; returns the failure count.
int runMidiImportChecks();

}
