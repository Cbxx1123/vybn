#pragma once

namespace tests {

class Checker;

// The plugin suite's VST3 checks (vybn-test-plugins.vst3 through the sandbox): scanning a bundle
// and a single file, a gain's audio, parameters, value texts, output changes, latency, its editor
// floating and embedded, state; an instrument's notes and its MIDI-mapped bend; a separate
// controller and an aux input as the sidechain. Records into the plugin suite's checker.
void runVst3Checks(Checker &c);

}
