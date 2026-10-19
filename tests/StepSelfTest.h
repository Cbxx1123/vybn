#pragma once

namespace tests {

// Offline checks of the step sequencer's plain logic (StepSequence.h): the grid, steps, velocities,
// rows as patterns, shifting, swing, mute, the every, Euclidean and random patterns, and the drum
// names and rows. Prints PASS/FAIL per check; returns the number of failures.
int runStepChecks();

}
