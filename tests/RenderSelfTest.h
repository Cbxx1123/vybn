#pragma once

namespace tests {

// main --render-selftest [--update-goldens] [--scene NAME]
//
// Renders scenes off screen (a hidden window, engine::Renderer::beginOffscreen) and compares each
// picture with its golden image, tests/render/<scene>.png (VIBIN_TEST_DIR "/render"). A picture
// matches when every pixel's red, green and blue are each within 2 of the golden's and no more than
// 0.05% of pixels differ by more; a missing golden fails. On a mismatch it writes
// <scene>.actual.png and <scene>.diff.png (differences in red, magnified, over a faded copy) into
// %TEMP%\vibin-render and names them in the FAIL line. --update-goldens writes every picture as its
// golden instead (PASS lines say "updated"). --scene renders that scene alone. PNGs are opaque RGB.
//
// Scenes, each at a fixed size, with the theme named:
//   shapes        640x480  every Shape x Elevation x Finish at depth scales 0.5 and 1, accent, ink and
//                          a scrim, on Theme::light()
//   shapes-dark   640x480  the same on daw::studioTheme(true) (the app's Carbon look)
//   text          640x480  every TextStyle x TextTone, the three alignments, a long line cut by a
//                          clip, studio dark
//   clips         480x360  nested clips cutting raised surfaces' shadows, text and an inset well
//   image         480x360  a generated ImageData (gradients and a checker) at its size, stretched,
//                          at half opacity and clipped
//   lines         640x480  rows of lines at widths 1 to 6 and angles 0 to 90, a row at opacity 0.3,
//                          a sine and a zigzag polyline, a line cut by a clip and a dot, studio dark
//   plots         640x480  a sine as a Line over a Fill to its middle, a decaying envelope as a Fill to the
//                          bottom under a Line, a waveform as a Range, a 4096-value Line, a Line cut by a
//                          clip and a two-value Line, studio dark
//   controls      900x600  a Context tree of the UI layer's controls (Button variants and sizes,
//                          Toggle on and off, Slider, Checkbox, RadioGroup, SegmentedControl,
//                          Stepper, Knob, ProgressBar, TextField, Chip, Badge, LevelMeter at a
//                          level, ListItem selected and not, a Panel), studio dark
//   arrangement   1200x520 daw::ArrangementView on daw::demoProject(), studio dark
//   pianoroll     1200x520 daw::PianoRollView on the demo project's first instrument clip
//   steps         1000x420 daw::StepSequencerView on a drum clip (a kick, snare and hat beat)
//   synth         1180x860 daw::SynthView on the synthesizer's Init setting (no audio device)
//   scroll        600x400  a ScrollView of 2000 ListItems (culling: only the rows in view are drawn)
//
// A scene with a component tree is rendered by a ui::Context on the hidden window: the window is
// placed far off the desktop so the real mouse never hovers it, and frames (update, then
// buildFrame) run until the fingerprint is the same three frames running (at most 300), so glides
// and animations have settled, before the frame is drawn off screen.
//
// After the comparisons it prints an INFO line per scene with what the frame cost and drew (from
// ui::FrameStats: build ms, surfaces, texts, glyphs, prims (lines and plots among them), draws,
// bytes uploaded, submit ms, plot values), so refactors can compare. Returns the number of failures (0 is a pass).
int runRenderSelfTest(int argc, char **argv);

}
