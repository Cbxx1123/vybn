#pragma once

namespace tests {

// Offline checks of the built-in synthesizer (Synth): the wave bank's levels and harmonics, pitch
// (octave, semitone, fine, bend), the amp envelope, the filters, unison, the voice modes and glide,
// the LFOs and the mod wheel, all notes off, output limits and the time a busy render takes, the cycle the panel draws,
// and the user presets (a round trip through a temporary folder).
// Prints PASS/FAIL per check; returns the process exit code.
int runSynthSelfTest();

}
