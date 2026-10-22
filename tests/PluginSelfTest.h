#pragma once

namespace tests {

// Offline checks of the plugin system through its real sandbox, with the project's own test
// plugins (vybn-test-plugins.clap): scanning a .clap, a gain effect's audio, parameters (set by
// the app, reported back by the plugin, shown as text) and state round trip, a sine instrument's
// sample-accurate notes, the sandbox stopping a plugin writing to the user's profile, and the app
// surviving a plugin that crashes, hangs or runs too slowly (silence, then the watchdog's
// verdict), plus the PluginManager scanning a folder. Prints PASS/FAIL per check; returns the
// process exit code.
int runPluginSelfTest();

}
