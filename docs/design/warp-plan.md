# Audio warping: plan

Status: built (stages 1-4), with the differences under "As built". Decided with the user: warping only (of the three ideas offered: warping, freeze and
bounce, modulators); imported and dropped files are warped when they are loops (their length fits whole
bars at a tempo found in them), else left as they are, with a setting; recordings are warped at the
tempo they were recorded at; turning warp on picks the mode from the file (Beats for percussive sound,
Complex for the rest).

## Problem

An audio clip plays its file at the file's own speed (`AudioClipPlayer`: "tempo changes don't stretch
them"). A drum loop at 120 BPM in a 96 BPM song drifts out of time at once; a song whose tempo map
changes, or ramps, leaves every audio clip where it was in seconds while the bars move under it; a
recording made at one tempo can't follow the tempo when it is changed later; and nothing can move a
late hit onto the beat or play a loop in another key. Every DAW this one is measured against has an
answer (Ableton's Warp, Logic's Flex Time and Flex Pitch, Bitwig's stretch modes, Cubase's AudioWarp,
Studio One's Audio Bend): audio that follows the bars, markers that pin a moment in the sound to a
beat, and stretching and transposing apart from each other.

## Terms

- A clip is **warped** when its sound follows the bars: its time is counted in beats, not seconds.
- The clip's **content beats**: a warped clip's own timeline, the beats its sound is laid out on. Beat
  `c` of the content plays at timeline beat `start + (c - c0)`, where `c0` is the content beat at the
  clip's start.
- A **warp marker** pins a moment of the file (seconds) to a content beat. Between two markers the
  sound is stretched evenly; outside them it plays at the clip's **tempo** (`bpm`, the content beats
  per minute of the file).
- A **transient**: the start of a hit or a note, found in the sound (the analysis below).
- **Stretch**: playing the sound faster or slower without changing its pitch. **Transpose**: changing
  its pitch without changing its timing.

## Model

In `src/audio/Warp.h` (namespace `audio`), since the audio thread plays it and the project keeps it,
as automation lanes are:

```cpp
enum class WarpMode : uint8_t { Repitch, Beats, Tones, Complex };
enum class WarpPreserve : uint8_t { Transients, Quarter, Eighth, Sixteenth };   // Beats: where it slices
enum class WarpLoop : uint8_t { Off, Forward, BackAndForth };                     // Beats: a slice too short
struct WarpMarker { double seconds = 0.0; double beat = 0.0; };                    // file seconds, content beats

struct Warp {
    bool on = false;
    WarpMode mode = WarpMode::Complex;
    double bpm = 0.0;                 // the file's tempo; 0 = not known (the clip was never warped)
    std::vector<WarpMarker> markers;  // strictly rising in both, at most kMaxWarpMarkers (4096)
    double pitch = 0.0;               // semitones, -48 to 48 (cents are its fraction); not in Repitch
    WarpPreserve preserve = WarpPreserve::Transients;
    WarpLoop loop = WarpLoop::Off;
    float envelope = 100.0f;          // Beats: each slice's decay, 0-100 (100 none)
    float grain = 50.0f;              // Tones: the grain, 0-100 (short to long, about 12 to 90 ms)
};
bool operator==(const Warp &, const Warp &);
```

- The map, allocation-free (the audio thread uses it): `warpSeconds(warp, beat)` (content beat to file
  seconds) and `warpBeat(warp, seconds)` (the inverse). No markers: `seconds = beat * 60 / bpm`. One
  marker: that line through it. Two or more: straight lines between them, the clip's tempo outside them.
  A binary search, then a line.
- `Clip::warp` (an `audio::Warp`). `Clip::offset` keeps its meaning, the file's seconds at the clip's
  start, warped or not: so `c0 = warpBeat(warp, offset)`, and the code that trims, splits, slices and
  comps changes only where it works out a new offset.
- `daw/model/Warp.h/.cpp`, the rules, as functions on a clip and the tempo map:
  - `clipSourceSeconds(clip, timing, beat)`: the file's seconds playing at a timeline beat (warped:
    through the map; else `offset + secondsAt(beat) - secondsAt(start)`), and `clipBeatAt(clip, timing,
    seconds)`, the inverse. `fileStartBeat`/`fileEndBeat`: where the file's start and end fall on the
    timeline. Every place that worked the offset or the file's end out by hand uses them: `splitClip`,
    `trimStart`, `sliceClip` and the comp's joins in `Takes`, the bridge's comp crossfades, the arrangement
    and audio editor waveforms and trims, recording.
  - `setWarp(clip, timing, on)`: turning warp on keeps the clip sounding as it does now (markers that
    follow the tempo map over the clip, at the tempo at its start: `followTiming`), so nothing moves
    until the tempo does; turning it off keeps the clip's start and offset and gives it the length its
    sound now lasts (cut to its file's end).
  - `setTempo(clip, bpm)` (scales the markers' beats about the clip's start, so the start stays put),
    `doubleTempo`, `halveTempo`; `fitToLength(clip, beats)` (stretches the clip's content to fill a
    length: Alt-dragging a warped clip's end).
  - Markers: `addMarker(clip, timing, beat)` (at the file's moment playing there now, so nothing moves),
    `moveMarker(clip, index, beat)` (between its neighbours: the sound either side stretches),
    `slipMarker(clip, index, seconds)` (moves the sound under a marker, the beat staying),
    `removeMarker`, `clearMarkers` (keeps the clip's tempo), `warpStraight(clip, timing, from)` (from a
    marker or beat on, one tempo: markers after it go), `quantiseMarkers(clip, timing, grid, strength)`.
  - `quantiseAudio(clip, timing, analysis, grid, strength, threshold)`: a marker at each transient (above
    the threshold) moved toward its nearest grid line by `strength`: audio quantise.
  - `guessWarp(analysis, seconds, beatsPerBar)`: the tempo, the first downbeat and whether it is a loop
    (below), which `applyGuess(clip, ...)` uses (a marker at the first downbeat on its beat, the tempo).
  - `guessMode(analysis)`: Beats when the sound is percussive (strong transients often), else Complex.
- History: `warp` is clip content: `sameClip`, `sameClipContent`, `sameSavedClip`, the bridge's
  `hashClips`. Undo names "Warp clip", "Unwarp clip", "Warp mode", "Clip tempo", "Move warp marker",
  "Add warp marker", "Remove warp marker", "Quantise audio", "Transpose clip". A `static_assert` on
  `sizeof(Clip)` joins `Track`'s, so the next field added has to be listed.

## Analysis

`audio/AudioAnalysis.h/.cpp`: what the sound holds, worked out once a file on a Background job and
kept by the `AudioPool` beside the file (`analysis(path)`, null until it is done; its arrival moves the
pool's `version()`, so the bridge sends the clips again with their slices).

- Transients: the sound folded to mono, a 1024-point FFT every 256 frames (Hann), the spectral flux of
  the log magnitudes (rises only), an adaptive threshold (the local median and mean over half a second,
  plus a floor) and peak picking at least 40 ms apart; each place refined to the frame where the energy
  over 64-frame windows rises fastest just before it. `Transient { frame, strength 0-1 }`.
- Tempo: the onset strength's autocorrelation over 60-200 BPM, weighted toward 120 on a log scale, the
  best lag refined between its neighbours; then, if the file's length is close (within 3%) to a whole
  number of bars (1, 2, 4, 8, 16 or 32 of 4/4) at a tempo within 70-180 BPM near the guess (or its half
  or double), that tempo (the file is a loop: `loop = true`). The first downbeat is the first strong
  transient within the first beat (else 0).
- Percussiveness: the share of the onset strength in the peaks (high for drums, low for pads and
  speech), which `guessMode` reads.
- Streamed files are decoded again on the analysis job (miniaudio, a piece at a time), so analysis never
  touches the stream's pool; recordings are read from their samples or their file the same way.
- The analysis is kept in memory with the file (in the pool's cache, freed with it); it isn't saved.

## Playback

`AudioClipPlayer` plays warped regions through stretch voices; unwarped ones exactly as now.

- `ClipRegion` gains the warp (`warp`, `contentStart` = `c0`), the clip's id (`key`), and for Beats its
  slices (`slices`, file frames and content beats, from the transients or the grid, made by the bridge).
- Each block a region under the playhead finds the file position for any frame from the transport:
  timeline beat, content beat, the map, file frames (`WarpPath::at(frame)`, a few tempo-map lookups a
  block, interpolated in between).
- **Repitch**: the file is read at the speed the map gives (a 4-point Hermite interpolation), so pitch
  goes with speed, as tape does. No state.
- **Beats**: each slice starts at the beat its first frame is pinned to and plays at the file's speed
  (transposed by resampling); a slice whose time is longer than its sound ends there, faded over 4 ms,
  or loops its last half forward or back and forth (`loop`); a slice cut short by the next crossfades
  into it over 3 ms. `envelope` fades each slice over its time. Best for drums.
- **Tones**: WSOLA. Hann grains (about 12 to 90 ms by `grain`), half overlapping, each read from where
  the map says at the grain's middle, nudged within a quarter grain to where it best continues the grain
  before (normalised cross-correlation on a decimated mono copy), read at the transposed speed. Best for
  monophonic parts, voice, bass.
- **Complex**: a phase vocoder. 2048-point frames (4096 above 64 kHz), 4 times overlapping; each frame
  reads the file around where the map says (at the transposed speed) and one hop of grain-time earlier,
  so the true frequency of each bin comes from the pair and needs no history: time-varying stretch,
  jumps and edits mid-play are all one case. Phases are locked to their peaks (identity phase locking)
  and taken from the channels' middle so the stereo image stays, and reset at transients so hits stay
  sharp. Best for full mixes, pads, chords.
- The file is read ahead (the map is known in the future), so a stretch voice has no latency: a cold
  start fills its overlap at once from the frames before. Nothing needs compensating.
- Where a warped clip plays at its file's own speed and pitch (ratio 1, no transposition), every mode
  plays the file directly, sample for sample as an unwarped clip does; switching between that and
  stretching crossfades over 5 ms. So a recording warped at the tempo it was made at plays exactly as
  recorded.
- Voices: a sequence with stretched regions owns up to four `StretchVoice`s (made with it on the main
  thread, sized for the largest frame), given to regions as they come under the playhead and taken
  back after. A new sequence (an edit) arrives with fresh voices; the audio thread swaps its playing
  voices into the new one by clip id and mode, so dragging a marker while it plays doesn't restart the
  stretcher, and the old sequence retires with the fresh ones for `collect()` to free. A transport jump
  or loop resets a voice (filled again at once).
- Streaming: the region asks ahead for the chunks its path reaches within half a second, as now; a read
  of a chunk not in memory gives silence for that grain or slice, keeping time.
- The audio thread never allocates or locks: voices, their FFT tables and buffers, and the player's
  scratch buffers are made on the main thread (`prepare` and the sequence's construction).
- Exports render through the same players on the offline engine, so they warp exactly as playback does.

## The file

In a `[[tracks.clips]]` table of an audio track, only when the clip has a tempo (has been warped):

```toml
warp = { on = true, mode = "complex", bpm = 120, pitch = -2.5, markers = [ { seconds = 0.012, beat = 0 }, { seconds = 2.004, beat = 4 } ] }
```

with `preserve = "transients" | "1/4" | "1/8" | "1/16"`, `loop = "off" | "forward" | "back and forth"`,
`envelope` and `grain` only when not their defaults. `mode` is `"repitch" | "beats" | "tones" |
"complex"`; an unknown mode or preserve or loop name is refused with a reason (as other enums are);
`bpm` is clamped to 20-999 (0 when missing: warp then can't be on), `pitch` to -48-48, markers are sorted
by beat, a marker that doesn't rise in both is dropped, and past 4096 the rest go. Older files have no
key and play unwarped, as they did; an older build reading a new file ignores the key and plays the clip
unwarped. The format number stays 3. Take lanes' clips carry it too (the same clip writer).

## The audio editor

The editor gains a toolbar row (in its panel, as the piano roll's) and marks on the waveform.

- Toolbar: Warp (on/off), the mode (a dropdown), the tempo (a `ScrubField`, tenths with Shift) with
  x2 and /2, Guess tempo, Transpose (semitones) and Detune (cents) scrub fields (greyed in Repitch), and
  the mode's own settings: Beats' Preserve and Loop and Envelope, Tones' Grain. A Warp menu: Warp from
  here (straight), Quantise audio... (a dialog: the grid, strength, threshold), Clear markers, Set the
  song's tempo from the clip.
- While warped the waveform is drawn through the map (each column's beats to the file's seconds), so it
  lines up with the bars, and the file outside the clip with it.
- A marker strip under the ruler: each marker a raised tab with a line down through the waveform. Drag
  one to move it in time (the sound either side stretches; snapped to the grid, Shift free); Alt-drag to
  slip the sound under it instead; double-click the strip to add one; double-click a marker or press
  Delete to remove it; right-click for its menu (Delete, Warp from here, Warp straight from here).
- Transients: faint ticks along the waveform's top. Hovering one shows a ghost marker; dragging it
  makes it a marker and moves it (Ableton's pseudo-markers). Hovering the waveform elsewhere shows the
  ghost at the mouse.
- A readout while dragging (the beat, the tempo between its neighbours as a percentage of the clip's).
- Unwarped, the editor is as it is now, with Warp in the toolbar.

## The arrangement

- A warped clip's waveform is drawn through the map; a small "W" badge (and its mode's short name when
  there is room) marks it; a transposed one shows "+3 st".
- Alt-dragging a warped clip's end stretches its content to fit (`fitToLength`); without Alt it trims as
  now (its end not past the file's end, through the map).
- The clip menu gains Warp (a toggle), Warp mode (a submenu), Double tempo and Halve tempo, Guess tempo,
  Quantise audio..., Transpose... and Set song tempo from clip.

## Import and recording

- Import and drops (`importAudioFiles`, the browser's Add to track): with Settings > Recording's "Warp
  imported loops" on (the default), a file whose analysis says it is a loop comes in warped at its tempo,
  its first downbeat on the beat it is placed at, in `guessMode`'s mode, and as long as its sound lasts in
  the project's tempo. Analysis takes a moment, so the clip is placed unwarped and warped when the
  analysis arrives (one edit, merged into the import's undo step when it comes within the same frame
  run; otherwise its own "Warp clip" step).
- Recording: a recorded clip is warped (`setWarp(..., true)` once its file is written: markers that
  follow the tempo map as it was while recording, Complex mode), so it plays as recorded and follows a
  tempo change made later.

## Tests

- audio suite: the map (both ways, outside the markers, one marker); Repitch at 2x reads every second
  frame; ratio 1 plays the file sample for sample in every mode; a 440 Hz sine stretched to 1.5x in
  Tones and Complex stays at 440 Hz (zero crossings) and lasts 1.5x; transposed +12 in Complex is 880
  Hz at the same length; Beats keeps a click train's clicks on their new beats; a voice survives a
  sequence swap (no reset, continuous output); a jump resets; streaming underruns keep time; no
  allocation on the audio thread (the voices' buffers' addresses don't change across blocks).
- project suite (or a new `WarpSelfTest.cpp` run from it): the analysis finds a click train's clicks
  within 2 ms and its tempo; a loop's length; `setWarp` on and off keeps the sound in place; markers
  added, moved, slipped and removed; `setTempo`, double and halve; `quantiseAudio`; split, trim and
  slice of warped clips keep the sound in place; the file round trip, older files, bad modes refused,
  clamping; undo labels; `sameSaved`.
- export suite: a warped clip renders the same offline as the live engine plays it.
- render suite: the audio editor with a warped clip, its markers, transients and the toolbar; the
  arrangement with a warped clip's badge. Goldens written only after looking.

## Stages

1. The map, `Warp`, the analysis, and the model's rules (`daw/model/Warp`), with their checks.
2. The stretch voices and the player (Repitch, Beats, Tones, Complex), with the audio checks.
3. The file, the history, the bridge, the takes and split/trim paths, import and recording.
4. The audio editor, the arrangement, the menus and the dialog; render scenes; the docs.

## As built

- Stages 1-4 as planned: `audio/Warp`, `audio/Fft`, `audio/AudioAnalysis`, `audio/Stretch` and the player;
  `daw/model/Warp` and its uses; the file, the history, the bridge, recording and import; the editor's
  strip, the warp row (`DawWarp.cpp`), the menus and the Quantise audio dialog; `--demo warp`.
- The analysis is worked out on demand rather than with every load: `AudioPool::analysis(path)` starts
  it once the file is in (the bridge asks for Beats and Complex clips, the editor for the clip shown), so
  files that are never warped cost nothing. An import looks at a file of up to two minutes at once
  (`analyseNow`), so a loop comes in warped in the same edit and undo step as the import, instead of
  arriving later as its own step.
- Onsets needed two corrections found by the checks: a frame is placed by the energy's steepest rise
  over its whole span (the log magnitudes rise as soon as a hit enters a frame, not at its middle), the
  file's first frame counting (a hit at frame 0), and a peak must stand well clear of the flux's median
  (`5 x median + 20`), which a held chord's flux never does (it had 70 false transients before).
- Beats: a slice whose time outlasts its sound fades out in the last 4 ms before its end (reading past
  it played the next hit early), and a new slice starts at full level with the one before fading out
  under it, so hits keep their attack. The slot a slice has is worked out when it starts.
- A followed tempo ramp gets a marker every eighth of a beat (every beat was 10 ms out in a 90-140 BPM
  ramp; an eighth is 0.18 ms).
- Moving a marker anchors the clip's start first (a marker at its offset and content beat), since the
  start's moment of the file is what keeps a clip in place; that anchor doesn't move, and slipping it
  moves the clip's offset with it. Delete doesn't remove a marker (the action belongs to clips there);
  a double-click or the marker's menu does.
- The arrangement shows a badge ("Beats +3 st") only on a warped clip that is stretched or transposed,
  so recordings, which are all warped, look as before. Transposition is in the warp row, not a dialog.
- Not checked by a test: a streamed file's underrun while stretching (it reads silence for that grain
  or slice and keeps time, as unwarped clips do), and the audio thread's lack of allocation, which is by
  construction (the voices and the player's buffers are made on the main thread; no `std::vector` is
  resized in `Stretch.cpp`'s render paths).
- Measured (`VYBN_TEST_BENCH`, Debug build with the audio code at -O3, 10 s of stereo stretched by 1.5
  and transposed): Repitch 9.8 ms, Beats 13.4 ms, Tones 63 ms, Complex 242 ms, so one Complex clip takes
  about 2.4% of a core while it plays and a Tones clip 0.6%.
- Checks: 21 in the audio suite (`runWarpAudioChecks`), 22 in the project suite (`runWarpModelChecks`, the editor's strip among them),
  and the render suite's `warp` scene.
