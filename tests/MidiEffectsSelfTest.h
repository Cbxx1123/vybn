#pragma once

namespace tests {

// Live MIDI effects (docs/design/midi-effects-plan.md). The audio checks: each effect on fixed events
// (the arpeggiator's steps, octaves and gate; a chord and its clean ends; the scale; the echoes; the
// velocity curve) and an arpeggiator before the synth through a ChainInstrument, its notes heard on the
// grid. Run by the audio suite.
int runMidiEffectAudioChecks();
// The model's: the file round trip and refusals, and the history's labels. Run by the project suite.
int runMidiEffectProjectChecks();

}
