#pragma once

namespace tests {

// Checks audio::TempoMap: beats to seconds and back through held tempos and ramps, bars in mixed
// signatures, tidying what it's given. Prints PASS/FAIL per check; returns the failure count.
int runTempoChecks();

}
