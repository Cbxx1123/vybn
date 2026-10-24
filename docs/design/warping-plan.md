# Audio warping: plan

Status: built, with the differences under "As built".

## Problems

- An audio clip plays at its file's own speed (`AudioClipPlayer`: "tempo changes don't stretch
  them"). A loop recorded or downloaded at 96 BPM drifts against a 120 BPM song, and changing the
  song's tempo leaves every audio clip where its seconds were.
- Nothing can change a clip's pitch without changing its speed, or its speed without its pitch.
- Nothing finds where a sound's hits are (its transients), so a drum loop can't be lined up with
  the grid, and a file's tempo is never known.

## The model

- `audio/Warp.h`: `WarpMode` Off (as now), Repitch (follows the tempo by playing faster or slower,
  so the pitch follows the speed), Beats (cut at the transients, each hit played at its own speed
  and stretched in between: drums and anything percussive) and Tonal (a phase vocoder: chords,
  voices, pads). `WarpMarker { seconds, beat }` pins a place in the file to a beat of the file's
  own count (its beat 0 is where the file's first beat would be). `ClipWarp { mode, bpm, transpose,
  markers }`: the file's tempo outside the markers, a transpose in semitones (fractions are cents;
  Beats and Tonal only, -24 to +24) and the markers, sorted with both fields increasing.
  `warpBeatAt(warp, seconds)` and `warpSecondsAt(warp, beat)` go between them: straight lines
  between markers, the bpm beyond them.
- `Clip::warp` (an `audio::ClipWarp`). A warped clip's `offset` stays seconds into the file at the
  clip's start; its file beat there is `warpBeatAt(offset)`, so timeline beat `b` plays file beat
  `warpBeatAt(offset) + b - start`. `daw/model/ClipTime.h`: `clipFileSeconds(timing, clip, beat)`
  and `clipBeatOfFile(timing, clip, seconds)` are the one mapping for every clip, warped or not;
  `splitClip`, `trimStart`, the takes' slicing, the arrangement's and editor's waveforms and their
  trims use them instead of the tempo map's seconds.
- Edits keep what is under the clip: `setWarpMode` (on: the clip's length in beats becomes the
  audio it covered; off: back to its seconds), `setWarpBpm` (the clip's start keeps its audio and
  its length follows), `setWarpMarkers` (the file beat at the clip's start is kept, so moving a
  marker only stretches the audio either side of it).
- Saved in `[[tracks.clips]]` when on: `warp = "beats"`, `bpm`, `transpose` (when not 0) and
  `warp_markers = [{ seconds, beat }, ...]`. Unknown modes refused; numbers clamped; markers that
  don't increase dropped. The format number stays 3 (an older build ignores the keys).
- Content for the history ("Warp clip", "Change clip tempo", "Transpose clip", "Move warp marker",
  "Add warp marker", "Delete warp marker"), the saved-state checks and the audio hashes.

## The sound

- Stretching runs offline, never on the audio thread. `audio/Fft.h`: a radix-2 real FFT with its
  tables made up front. `audio/Stretch.h`: `stretchAudio(source, path, mode, pitch, transients,
  cancel)` where a `TimePath` gives, for each moment of the output, the place in the source it
  plays (sampled every 5 ms, linear between), so tempo ramps and markers are one curve.
  - Repitch: four-point interpolation along the path.
  - Tonal: a phase vocoder (4096-point Hann frames, a quarter hop) whose analysis frame is taken
    wherever the path says, its instantaneous frequencies measured from a second frame a hop
    earlier (so any analysis hop works), identity phase locking around the spectral peaks
    (Laroche and Dolson), the stereo image kept by rotating both channels by the mid channel's
    phase change, and the phases reset at transients so attacks stay sharp.
  - Beats: the file cut at its transients; each piece starts exactly where its transient lands and
    is time-stretched by WSOLA (1024-point frames, half hop, the best of a +-256 sample search
    by cross-correlation), normalised by its window sum so the attack comes out as recorded,
    with a 1 ms fade into the next piece and a 4 ms tail.
  - Transpose: the stretch is made `pitch` times longer and resampled by `pitch`.
- `audio/Onsets.h`: `detectTransients` (log spectral flux over 1024-point frames, a 256-sample hop,
  an adaptive median threshold, peaks at least 40 ms apart, each refined to the sharpest rise of
  the envelope), `estimateTempo` (autocorrelation of the flux between 60 and 200 BPM, weighted
  toward 120, folded into 80-160), `loopTempo` (a file whose length is a whole 1, 2, 4, 8 or 16
  bars of 4/4 within 3% of the estimate gets that exact tempo) and `bpmFromName` ("Loop 120bpm",
  "kit_96_", "Groove 87.5 BPM").
- `AudioPool::warped(request)`: the stretched sound for a clip, made on a Background job and kept
  in a `ResourceCache` (swept as the sound files are). A render covers the whole file at the
  clip's placing, so trimming, splitting and, with one tempo, moving the clip reuse it; its key is
  the file, the warp, and the tempo (its BPM, or the map and where the file's beat 0 lands when the
  tempo changes). While one is being made the clip plays the last render made for it (a marker
  being dragged sounds as it was) or, with none, is silent. `version()` goes up as one arrives.
  `transients(path)` detects a file's transients once, on a job, for the editor.
- `AudioBridge::clipSequenceFor` gives a warped clip a region of its render, whose seconds are the
  timeline's, so `AudioClipPlayer` is unchanged. `renderReady` waits for every warped clip's
  render, so exports and freezing play the exact sound.

## The editor

- The audio editor's toolbar: a Warp toggle, the mode (Repitch, Beats, Tonal), the clip's BPM (a
  scrub field, with x2 and /2), Transpose (semitones; fine drags are cents) and a Warp menu:
  Detect tempo, Tempo from the clip's length (1-16 bars), Snap transients to the grid, Clear warp
  markers, Use the clip's tempo for the song.
- Under the ruler, while warped, a warp strip: the transients as ticks and the markers as raised
  pins. Double-click the strip to add a marker (on the nearest transient within 6 px); drag a
  marker to move its beat (snapped to the grid, Shift for free), the audio either side stretching;
  double-click a marker to delete it. The waveform is drawn through the warp, so it lines up with
  the bars as it plays.
- The arrangement draws warped clips through the warp too, and a clip's menu has Warp and Detect
  tempo.
- Import: a short file (60 s at most) whose name holds a tempo, or whose detected tempo fits whole
  bars, comes in warped (Beats) at that tempo (Settings > Audio: Warp loops on import, on).

## Tests

- Audio suite: the FFT against a direct DFT; Repitch of a sine at 2x is an octave up and half as
  long; Tonal at 0.5x and 2x keeps a sine's frequency within 1%; Beats keeps clicks' places within
  1 ms; transpose by +12 doubles a sine's frequency without changing its length; transients of a
  click track are found within 2 ms; the tempo of a 120 BPM click loop is found; `bpmFromName`.
- Project suite: the warp map and its inverse; `clipFileSeconds` against the old arithmetic for
  unwarped clips; split and trim of a warped clip keep its sound in place; the file round trip;
  the history's labels.
- Bridge: a warped clip plays its render (the pool's), the region's offset lines up the clip's start.

## As built

- A stretched sound is kept in RAM as floats (a minute of stereo at 48 kHz is about 23 MB), not
  streamed through the sample cache; the resource manager lets it go once no clip plays it.
- `setWarpMarkers` keeps the file beat at the clip's start by moving `offset`, so the clip's start can
  play a little earlier or later in the file when a marker either side of it moves: the marker lands
  exactly where the mouse put it, which matters more.
- Import: a tempo in the name counts at once when "bpm" is written beside it; a bare number ("kit_96_")
  only when the file's length is whole bars at it, so a "Snare 100" one-shot doesn't come in warped.
- Measured (audio suite): Tonal keeps a 440 Hz sine at 440 Hz and its level within 0.002 dB at 0.5x and
  2x; Beats keeps clicks within 0.17 ms at 0.8x; transients are found within 0.9 ms; the export renders
  a warped 60 BPM loop at 120 BPM with its clicks within 0.42 ms.
