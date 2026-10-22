# Tempo map and time signatures: plan

Status: implemented (stages 1-5 below), with the differences listed under "As built". Decided
with the user before starting: steps and ramps, denominators 2/4/8/16, audio clips stay on their
beat (no stretching), editing in arrangement lanes plus the transport bar.

## Problem

The project has one tempo (`Project::tempo`) and one bar length (`Project::beatsPerBar`, always over
4), and the tempo isn't even the project's: `DawApp` copies it from `audio::Transport` every frame,
because the transport bar edits the transport. Every conversion between beats and time is a single
multiplication (`samplesPerBeat()`, `secondsPerBeat()`, `position / spb`), written out in about 130
places: the players, the metronome, plugins' transport, recording, rendering, MIDI export, and every
grid and ruler. A song can't speed up, slow down, or change metre.

## Terms

- A **beat** stays what it is everywhere in the code: a quarter note. Clips, notes, automation, the
  loop and the selection keep their positions in beats, so none of that data changes.
- A **bar** is as long as its signature says: `numerator * 4 / denominator` beats (6/8 is 3 beats).
- **Time** is seconds from the start of the timeline; samples are seconds times the device's rate.

## Model: `audio::TempoMap`

In `src/audio/TempoMap.h/.cpp` (the audio thread reads it, the model owns it; `vybn_daw` already
links `vybn_audio`). A plain value: copyable, compared, saved.

- `TempoPoint { double beat; double bpm; bool ramp; }`: sorted, the first always at beat 0. A point
  holds its tempo until the next, or with `ramp` goes in a straight line (BPM against beats) to the
  next point's tempo. BPM 20-400 as now.
- `Signature { int bar; int numerator; int denominator; }`: sorted, the first always at bar 0,
  numerator 1-16, denominator 2, 4, 8 or 16. Changes are placed on bars, not beats, so changing an
  earlier signature moves the later changes with their bars (clips stay on their beats).
- Precomputed when it's built (`rebuild()`, called by every edit): each tempo point's start time,
  each signature's start beat. Queries are a binary search and a closed form, no allocation, so the
  audio thread can call them:
  - `seconds(beat)`, `beat(seconds)`: a held segment is linear; a ramp from `b0` with slope `k` BPM
    per beat is `t = 60/k * ln(bpm(b)/b0)` and back `b = s + b0/k * (exp(k*t/60) - 1)`.
  - `bpm(beat)`, `signature(beat)`, `barStart(bar)`, `barAt(beat)`, `barLength(bar)`.
  - `position(beat)` -> `{bar, beat in bar (in the signature's unit), fraction}`, which replaces
    `barsBeats(beat, beatsPerBar)` in `widgets/Format.h`.
  - `forEachBar(from, to, f)` for grids and rulers, `nearestBar(beat)` for bar snapping.
- With one point and one signature it gives exactly today's numbers (the constant case is tested
  against the old formulas), so stage 2 below changes nothing audible.

## Project, file and history

- `Project::tempo` and `beatsPerBar` become `Project::timing` (a `TempoMap`). `lengthBeats()` is
  `timing.barStart(bars)`. `NewProjectOptions` gets a denominator.
- The `.vbp` keeps `tempo` and `beats_per_bar` (the first point and the first signature's
  numerator), so older builds open new files with the song's opening tempo, and new builds open old
  files unchanged. New keys, written only when they say something:
  `tempo_changes = [{ beat, bpm, ramp }, ...]` (from the second point on), `signature_changes =
  [{ bar, numerator, denominator }, ...]` (from the second on), `tempo_ramp` when the first point
  ramps, and `beat_unit` for the first signature's denominator when it isn't 4. Read values are
  clamped and sorted, duplicates dropped.
- History: the map is one of the project's own fields (copied in the shell, compared in
  `sameContent`/`sameSaved`, restored by `ProjectStore::restore`). Labels "Change tempo" and
  "Change time signature". `HistoryRestore::tempo` becomes `timing`.
- `ProjectStore::setTempo` becomes `editTiming(f)`: one revision bump, and the audio side is told.
  The per-frame copy from the transport goes: the project is the source of the tempo again.

## Audio

- `Transport` drops its tempo atomic and holds the published map instead: the main thread hands it a
  new immutable `TempoMap` (shared, retired by stamp the way `DeviceChain` retires layouts) whenever
  the project's changes; `begin()` picks up the newest one.
- When a new map arrives the playhead keeps its **beat**, not its sample: `begin()` converts the
  position through the old map to a beat and back through the new one (unless a locate is pending).
  Editing the tempo while playing doesn't jump the music.
- `TransportState` gets `const TempoMap *map` (never null), `beatAt(frame)` and the new
  `frameAt(beat)` go through it, and `tempo` is the BPM at the block's start (for plugins and the
  synth's sync). `samplesPerBeat()` goes, so every caller is found by the compiler.
- The loop moves into the transport in **beats** (it already is in the project) and
  `framesBeforeLoop` converts through the map, so a tempo edit can't leave it at stale samples.
- `NotePlayer`: block start and end beats from the map, `frameOf(beat)` through it.
- `AudioClipPlayer`: an audio clip still plays its file at its own speed from its start beat (no
  time-stretching, as now); the time into the file is `seconds(beat) - seconds(clip start)`, and the
  crossfades use the map. `AudioBridge` rebuilds clip sequences when the map changes, as it does
  for the tempo now.
- Metronome: a click on each unit of the signature (eighths in 6/8), the accent on each bar's
  start, from the map. The count-in counts one bar of the signature at the start position.
- Plugins: the protocol's transport gets the signature (numerator, denominator), the bar's start
  beat and the tempo's slope, so the host fills CLAP's `tsig_num/denom`, `bar_start`, `bar_number`
  and `tempo_inc` properly.
- MIDI input stamps are converted to beats through the map on the main thread.

## Recording, rendering, export

- Recording: clip starts and lengths from samples through the map (`Recording.cpp`,
  `RecordingController`), the count-in's bar from the signature.
- `RenderJob`: frames for a range from `seconds(end) - seconds(start)`; tails stay in seconds.
- MIDI export: a tempo event per point and a signature event (`FF 58`, denominator as a power of 2)
  per change; a ramp is written as steps every 1/16 beat.
- Audio file import: a dropped file's length in beats is `beat(seconds(start) + file seconds) -
  start`.

## Views

- Every grid and ruler (arrangement, piano roll, audio editor, step sequencer) draws bars with
  `forEachBar` instead of `beat % beatsPerBar`, so bars of different lengths line up. Bar snapping
  snaps to bar lines. Readouts use `TempoMap::position`.
- **Arrangement**: two global lanes between the ruler and the first track, which can be hidden
  together (a view setting):
  - *Tempo*: the curve over the song (steps and ramps) with its points, a scale to its left in the
    header column. Click to add a point (snapped to the grid unless Shift), drag one to move it in
    beat and BPM, double-click or right-click it for a menu: ramp to next / hold, delete. The first
    point moves only in BPM.
  - *Signature*: a marker per change ("3/4"). Click a bar without one to add one there (copying the
    signature in effect), drag a marker along bars, double-click to edit numerator and
    denominator, right-click to delete. The first marker can't move or go.
- **Transport bar**: the tempo shows the BPM at the playhead; changing it changes the point in force
  there (so with one point, the song's tempo, exactly as now), one undo step per drag. The signature
  readout shows the one at the playhead and opens its editor.
- Project settings and New project: the opening tempo and signature (numerator and denominator).

## Stages

Each one builds, passes the suites and is its own commit.

1. `TempoMap` and its unit tests (conversions both ways, ramps against numeric integration, bar
   math with mixed signatures, the constant case against the old formulas). Nothing uses it yet.
2. The model, file and history on the map; every `beatsPerBar` and `secondsPerBeat` site goes
   through it with the constant map. Output identical: the render and export suites unchanged.
3. The audio thread on the published map: transport, loop in beats, players, metronome, plugins,
   recording, rendering.
4. Grids and rulers with bars of any length; the tempo and signature lanes; the transport bar.
5. MIDI export, the project settings, docs (`daw.md`, `audio.md`, the file keys).

## As built

- Stages 2 and 3 went in together: the transport's tempo atomic was the reason the project's tempo
  was copied from it, so the model and the audio side changed at once. `AudioBridge::sync` publishes
  the map to its engine's transport each frame (`Transport::setTiming` compares first), so the live
  app and exports are fed the same way; `DawApp`'s per-frame copy is gone.
- The transport retires old maps by counting blocks begun (freed two blocks after they were
  replaced), and `Transport::beats(rate)`/`sampleOf(beat, rate)` answer the main thread's questions.
- Audio clips: the time into the file is the block's sample time minus the clip's start time, so
  the per-sample work is no more than before (one `secondsAt` per clip per block).
- The metronome finds the unit each block starts in and the frame the next starts on, rather than
  asking the map every sample. The count-in takes the signature at the playhead from the main
  thread (`startCountIn(clicks, perBar, unit)`).
- The plugin protocol went to version 4 (the transport's new fields change the shared block).
- The lanes are their own component, `TimingLanes`, which the arrangement owns, lays out under its
  ruler and draws in its cache. Adding a signature is a double-click (a single click in that thin
  lane added changes too easily); the menus and the signature's editor are `DawApp`'s, through
  `TransportHooks`. The lanes can be hidden (View > Tempo and signature lanes); they are hidden in
  the render suite's older arrangement scenes, which keep their goldens, and shown in `tempo`.
- A `tempo` demo plays the render scene's map in the app; with `VYBN_VERIFY_CACHE=1` and
  `VYBN_VERIFY_STORE=1` it printed only FRAME lines over about 20 s of playback.

## Tests

- project suite: the map's maths; file round trips of a map with ramps and signatures; an old file
  opening; undo and redo of tempo and signature edits; `sameContent` seeing them.
- audio suite: a note after a tempo change starts on the right sample; an audio clip across a ramp
  plays continuously; the playhead keeps its beat when the map changes mid-play; the loop with a
  tempo change inside it.
- export suite: a render with a tempo change against the expected length; MIDI export bytes.
- render suite: a scene with the tempo and signature lanes and a 7/8 bar in the ruler.
