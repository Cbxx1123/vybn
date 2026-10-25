# Live MIDI effects: plan

Status: built, with the differences under "As built".

## Problems

- An instrument track's notes go straight to its instrument. The arpeggiator exists only as a note
  tool that rewrites a clip's notes; nothing arpeggiates, harmonises or echoes what is played live
  from a keyboard, or changes a part without rewriting it.

## The model

- `DeviceKind::MidiEffect` (`"midi"` in the file), ids `arpeggiator`, `chord`, `scale`, `echo` and
  `velocity`, with built-in parameter lists like the built-in effects' (`audio/MidiEffects.h`:
  `MidiEffectType`, `midiEffectInfo`, `midiEffectSetting`, `midiEffectFromKey`).
- `Track::midiEffects` (instrument tracks): `audio::DeviceSetting`s in order, at most 8, before the
  instrument. Content: saved as `[[tracks.midi_effects]]` tables (`writeDevice`/`readDevice`), undone
  ("Add MIDI effect", "Remove MIDI effect", "<effect> settings", "Move MIDI effect"), compared by the
  saved checks, listed in the history's hand-kept Track fields and `ProjectStore::restore`.

## The engine

- A `MidiEffect` is a `Device` (parameters, applySetting) that turns a block's note events into
  others: `process(in, out, clock)`, with `MidiEvent { frame, on, key, velocity }` and a `MidiClock`
  (the block's frames, rate, whether the transport plays, and the beat at any frame: the transport's
  when it plays, else a free-running beat at its tempo, so an arpeggio runs while stopped). Events it
  makes for later blocks (an echo, an arpeggio's note-off) wait in a fixed queue in absolute samples.
  Nothing allocates: every list is a fixed array.
  - Arpeggiator: pattern (Up, Down, Up-down, Down-up, As played, Random, Chord), rate (1/4 to 1/32 with
    triplets), octaves (1-4), gate (5-200%), swing, velocity as played or fixed, and Hold (latch). Its
    steps fall on the beat grid of the song, so it plays in time and exports the same every time; a
    random pattern draws from a hash of the step.
  - Chord: adds a shape's notes (major, minor, 7ths, sus, power, octave, dim, aug) with an inversion and
    a level for the added notes; counted per output key, so overlapping chords end cleanly.
  - Scale: moves each note to the nearest (or next up, or down) note of a scale on a root, remembering
    what each key became so its note-off follows.
  - Echo: repeats each note 1-8 times a synced time apart, each quieter by the decay and moved by a pitch
    step, note-offs echoed with their notes.
  - Velocity: a drive curve, the output range and a random amount (hashed, so it repeats).
- `DeviceChain` keeps the MIDI effects (`apply(midiEffects, instrument, effects)`, the layout's
  `midiEffects`), and `ChainInstrument` runs them: with any, it takes notes with their frames
  (`timedEvents`), runs the chain over the block's events in `process`, and hands the result to the
  instrument (timed, or splitting the block at the events for one that isn't). `allNotesOff` flushes
  them. Controllers and expression pass straight through.
- `AudioBridge` applies `Track::midiEffects` (none while frozen); exports play them the same.

## The interface

- The Devices tab on an instrument track: MIDI effect cards before the instrument (the same card as
  other devices: name, kind, knobs and choices, bypass, move left and right, remove), and "MIDI
  effect" (a menu of the five) to add one. A track's menu: Add MIDI effect.
- The full device view and automation of MIDI effects stay not done.

## Tests

- Audio suite: each effect on fixed events (the arpeggiator's notes on the grid in order up and down,
  its gate and octaves; a chord's notes and its clean ends; the scale's mapping and note-offs; echoes'
  times, levels and pitches; velocity's range), and a ChainInstrument with an arpeggiator before a
  recording instrument, timed and not.
- Project suite: the file round trip, refusals, the history's labels.

## As built

- The cards use a description of each effect (its parameter list) rather than the chain's own device,
  since a MIDI effect's parameters never change.
- Measured (audio suite): an arpeggiator before the synth plays eight sixteenths over two beats, each
  heard within 14 ms of the grid (the onset analysis's precision; the events themselves are sample exact).
