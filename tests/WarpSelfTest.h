#pragma once

namespace tests {

// Checks warping's audio side (docs/design/warp-plan.md): the FFT, the warp map, the analysis (a click
// train's transients, tempo and loop), and the clip player's warped regions in every mode (playing at
// the file's speed sample for sample, Repitch's speed, Tones' and Complex's stretch and transposition
// keeping or moving the pitch, Beats keeping hits on their new beats, voices kept across a new
// sequence). Run from the audio suite. Prints PASS/FAIL per check; returns the failure count.
int runWarpAudioChecks();

// Checks warping in the project: the map's rules on a clip (on and off, following the tempo map, the
// clip's tempo, markers added, moved, slipped and removed, quantising audio), splitting, trimming and
// slicing warped clips, the file round trip and its refusals, and the undo labels. Run from the
// project suite.
int runWarpModelChecks();

}
