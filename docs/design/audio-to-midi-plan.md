# Audio to MIDI: plan

Status: built, with the differences under "As built".

## Problems

- A recorded drum loop, a hummed or played melody, or a chord part can only be played as audio:
  nothing turns it into notes to put on another instrument, to edit, or to double.

## The conversions

`audio/Transcribe.h` (offline, on a background thread), each from a sound's samples:

- **Drums**: master's transients (`OnsetAnalyser`), and for each hit the energy in three bands
  (under 150 Hz, 150 Hz-4 kHz, over 6 kHz) and the noisiness over its first 60 ms: a kick when the lows
  lead, a hat when the highs do and it is short, a snare otherwise (open hats ring longer). General
  MIDI keys (36 kick, 38 snare, 42 closed hat, 46 open hat), velocity from the hit's strength.
- **Melody**: YIN pitch every 5 ms (the cumulative mean normalised difference, the first dip under a
  threshold refined by a parabola, 50 Hz-1.5 kHz), voiced where the dip is clear and the level is over
  a gate, a median filter over the track, then notes where the pitch holds within half a semitone and
  the level stays up, split at transients and at jumps; at least 60 ms. Velocity from the level; the
  pitch's drift within a note kept as its tuning expression (master's note expression), so a bend
  plays back.
- **Harmony**: a chromagram (an 8192-point FFT every 50 ms folded into 12 pitch classes, 55 Hz-2 kHz,
  plus the lowest strong class as the bass), averaged over each beat, matched against major, minor,
  7th, minor 7th, diminished and suspended templates; repeated chords merged; each written as its root
  in octave 3 and its chord tones above.

## In the app

- An audio clip's menu: Convert to MIDI > Drums, Melody, Harmony. It runs on a Background job (a
  note "Converting..."), then adds an instrument track under the clip's track (named "<clip> drums",
  "melody", "harmony", playing the built-in synth on a fitting preset), with one clip over the same
  beats. Times go through the clip's mapping (`clipBeatAt`), so a warped clip's notes land on its beats.
  One undo step ("Convert to MIDI").

## Tests

- Audio suite: generated drums (a low sine thump, a noise burst, a short high noise) classified and
  placed within 3 ms; a three-note sine melody at known pitches and lengths with a glide kept as tuning;
  C major, A minor and G7 chords found over four beats each.
- Project suite: the clip made from a conversion lands on the clip's beats (warped and not).

## As built

- Melody: where no dip falls under YIN's threshold, the deepest is taken while it is still clear (under
  0.35), and a transient splits a note only where the level really rises (by half over 30 ms): sounds
  ringing together beat, and master's onset analysis reads that beating as hits. Wavering splits of one
  key under 80 ms apart are joined, and a note's first and last 20 ms are left out of its tuning.
- Drums: hits under a tenth of the loudest are left out (a tail cut off by an edge clicks).
- Drums play a library instrument in the Drums category when there is one, else the synth's Synth Pluck.
