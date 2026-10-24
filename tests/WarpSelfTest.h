#pragma once

namespace tests {

// Warping (docs/design/warping-plan.md). The audio checks: the FFT, the three stretches (a sine's
// pitch and length, clicks' places), the transpose, transients, the tempo found from a beat and a
// file's name. Run by the audio suite.
int runWarpAudioChecks();
// The model's: the warp map, the one clip mapping, the edits that keep what is under a clip, split
// and trim, the render's path and placing, snapping transients, the file and the history's labels.
// Run by the project suite.
int runWarpProjectChecks();

}
