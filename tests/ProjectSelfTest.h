#pragma once

namespace tests {

// Offline checks of the TOML reader and the .vbp project format: a round trip of the demo project
// through text and through a file, values that must read back exactly, hand-written TOML, and
// files that must be refused. Prints PASS/FAIL per check; returns the process exit code.
int runProjectSelfTest();

}
