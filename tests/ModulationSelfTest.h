#pragma once

namespace tests {

// Modulators (docs/design/modulators-plan.md). The audio checks: an LFO's shapes, polarity, sync and
// free rate; Steps and its glide; the follower's times; a chain's effect parameter moved by a routed
// LFO over blocks, depth's sign and clamping, under automation; the track's volume moved. Run by the
// audio suite.
int runModulationAudioChecks();
// The model's: the file round trip and refusals, routes to missing modulators dropped, the history's
// labels, and the freeze hash following modulators. Run by the project suite.
int runModulationProjectChecks();

}
