#pragma once

#include <string>
#include <vector>

#include "daw/model/Project.h"
#include "audio/Transcribe.h"

namespace daw {

// Audio to MIDI in the project (docs/design/audio-to-midi-plan.md): what a conversion of an audio clip
// makes, laid on the clip's beats through its mapping (warped or not; daw/model/Warp.h)
enum class Conversion { Drums, Melody, Harmony };
const char *conversionName(Conversion kind); // "drums", "melody", "harmony"

// The file's seconds at each beat over the clip (its start, each beat after, its end): where Harmony
// cuts its chords
std::vector<double> clipBeatBoundaries(const Project &project, const Clip &clip);
// An instrument clip over the same beats as `audio`, holding the notes found in its file (seconds into
// the file) that fall inside it: their starts and lengths on its beats (drums a sixteenth long), and a
// melody's drift as each note's tuning curve
Clip transcribedClip(const Project &project, const Clip &audio, const std::vector<audio::TranscribedNote> &notes, Conversion kind);
// The same for chords: each played over its beats
Clip chordClip(const Project &project, const Clip &audio, const std::vector<audio::TranscribedChord> &chords);
// The track a conversion adds, named after the clip ("Loop melody"), playing `instrument`, holding `clip`;
// inserted after `track`, going where it goes. Returns its index.
int addConvertedTrack(Project &project, int track, Clip clip, const audio::DeviceSetting &instrument);

}
