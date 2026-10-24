#pragma once

#include <cstdint>
#include <string>

#include "daw/model/Project.h"

namespace daw {

// Freezing and bouncing tracks (docs/design/freeze-plan.md): the model's side. The render is the
// Freezer's (daw/audio/Freezer.h).

// Whether a track can be frozen or bounced: audio and instrument tracks (a bus's sound is others')
inline bool canFreeze(const Track &t) { return t.kind == TrackKind::Audio || t.kind == TrackKind::Instrument; }

// What a track's frozen sound depends on: the clips it plays (notes, controllers, audio and their
// warps), its instrument and effects (kind, id, values, bypass, sidechain and automation; not a
// plugin's opaque state), the tempo map, and the same of the tracks its effects are keyed by. Not
// its name, fader, pan, mute, solo, sends, output or volume and pan automation, which play after it.
uint64_t freezeHash(const Project &project, int track);
// Whether a track is frozen and its freeze is still what it would render
bool freezeCurrent(const Project &project, int track);

// A frozen track turned into an audio track playing its freeze from the song's start (its effects
// are in the sound, so they go; its fader, pan, sends, output and volume and pan automation stay).
// False when the track isn't frozen.
bool flattenTrack(Project &project, int track);
// A new audio track after `track` playing `file` (`seconds` long) from the song's start, named
// "<name> (bounced)", going where the track went; the track is muted. Returns the new track's index.
int addBounce(Project &project, int track, const std::string &file, double seconds);

}
