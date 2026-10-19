#pragma once

namespace tests {

// Offline checks of the export's mastering tools (audio::master): the K-weighting coefficients
// against the published 48 kHz values, EBU R128 loudness of sines and silence, true peak against
// sample peak, the true-peak limiter's ceiling, alignment and transparency on quiet material,
// dither's error and fades, the file encoders, and the MIDI writer (a small SMF reader in the test
// parses its output back). Prints PASS/FAIL per check; returns the process exit code.
int runExportSelfTest();

}
