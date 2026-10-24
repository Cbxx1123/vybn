# The Analyzer panel: plan

Status: built, with the differences under "As built".

## Problems

- The only live view of the sound is the mixer's peak meters. Nothing shows its spectrum, its
  stereo image or its loudness while it plays; loudness is only measured after an export.

## The taps

- `audio/Analysis.h`: `AnalysisTap`, a lock-free ring of stereo frames (32768 frames) that audio
  threads write and the main thread drains. Writing is a try: a writer that finds another writing
  skips its block (counted), so a tap moved between tracks can't have two writers; nothing waits.
  `enabled` (off while the panel is hidden) costs one atomic load per block.
- The `AudioSystem` owns two for its life (so they outlive every processor that writes them):
  `masterTap()`, written after the master volume and gain (what is heard), and `trackTap()`, which
  the selected track's `TrackProcessor` writes after its fader and pan when `AudioBridge` points it
  there (`TrackProcessor::analysis`).

## The analysis (main thread)

- `SpectrumAnalyzer`: the newest N frames (2048 to 16384, default 8192) through a Hann window and
  the FFT each frame, the left and right powers averaged, as dB on 256 bands spaced by log
  frequency from 20 Hz to 20 kHz (a band narrower than a bin reads the interpolated bin), with a
  tilt (0, 3 or 4.5 dB per octave about 1 kHz), falling at 20 dB/s, and a peak hold (3 s, or
  until reset with Hold).
- `StereoMeter`: the correlation over the last 300 ms (+1 mono, 0 unrelated, -1 out of phase),
  the balance, the width (side over mid), and the last 1024 frames as points for the goniometer.
- Loudness: `master::LoudnessMeter` and `TruePeakMeter` fed from the tap: momentary, short-term,
  integrated (since Reset or the transport starting from stopped), the range and the true peak, and
  a 30 s history of the short-term values.

## The panel

- `AnalyzerView` (`daw/views/Analyzer.h/.cpp`), the Analyzer dock panel (a tab beside the Mixer;
  View > Analyzer, Ctrl+6). Its header: Master or Selected track, the FFT size, the tilt, Hold and
  Reset. The spectrum fills the left, in an inset well with frequency lines (20 Hz to 20 kHz) and
  level lines every 12 dB, the curve filled under its line and the held peaks as a faint line; the
  mouse shows the frequency, the nearest note and the level. On the right a goniometer (the points
  as dots, mid up and side across, scaled to the level), the correlation and balance as bars, and
  the loudness: M, S and I large, LRA and true peak, the short-term history as a line.
- The taps are enabled only while the panel shows; the analysis runs in its `handleInput`.

## Tests

- Audio suite: a tap passes frames in order, skips a second writer and reports drops when full;
  the spectrum of a 1 kHz sine peaks at 1 kHz within a band, at its level within 1 dB, and the
  tilt moves it by the slope; correlation of mono is +1, of inverted channels -1, of noise near 0;
  a 997 Hz sine at -20 dBFS reads about -23 LUFS momentary.
- Render suite: the panel with a fixed spectrum and goniometer (a golden).

## As built

- The master tap is written after the master volume and before the monitoring gain (the transport
  bar's output level, and `--silent`), so it measures the mix as an export renders it.
- The integrated loudness starts again on Reset and when the source changes, not when the transport
  starts (pausing would lose it).
- The short-term history and the goniometer's zoom follow the audio drained from the tap, not the
  frames drawn, so the picture is the same however fast the window draws (the render scene relies on it).
- The loudness through the view isn't checked separately: `LoudnessMeter` and `TruePeakMeter` are the
  export suite's, checked there.
