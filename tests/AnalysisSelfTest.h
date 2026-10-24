#pragma once

namespace tests {

// The Analyzer's measurements (docs/design/analyzer-plan.md): the tap (order, a second writer skipped,
// drops when full, off costs nothing), the spectrum of a sine (its band and level, the tilt, the fall
// and the hold), the stereo meter (mono, inverted, noise, balance) and the loudness of a sine through
// the view. Run by the audio suite.
int runAnalysisChecks();

}
