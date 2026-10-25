#pragma once

namespace tests {

// Audio to MIDI (docs/design/audio-to-midi-plan.md): generated drums (a kick, a snare, closed and open
// hats) found and named at their places; a sine melody's keys, starts and lengths, and a glide kept as
// tuning; C, Am and G7 chords found over their beats. Run by the audio suite.
int runTranscribeChecks();
// The clips a conversion makes land on the audio clip's beats, warped or not. Run by the project suite.
int runTranscriptionProjectChecks();

}
