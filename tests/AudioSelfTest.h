#pragma once

namespace tests {

// Checks the audio system without a device or speakers: renders offline through the same path
// the device callback uses and measures the result. Prints each check; returns 0 when all pass.
// Run with `main --audio-selftest`.
int runAudioSelfTest();

}
