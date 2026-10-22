# Markers, MIDI import and file drops: plan

Status: implemented (stages 1-5), with the differences under "As built". Decided with the user
before starting: a MIDI file's tempo map is asked about each time; a click on a marker moves the
playhead there; new tracks from MIDI play the built-in synth.

## Problems

- There are no markers: nothing names a place in the song, and the only ways to move the playhead
  are the ruler, the transport's start button and the loop.
- MIDI goes out (`MidiExport`) but never comes in.
- Nothing can be dragged into the app from Windows: the window never asks GLFW for dropped files.
  Audio comes in only through Import audio's file dialog and the browser.

## Markers

- `Marker { ObjectId id; double beat; std::string name; }` and `Project::markers`, sorted by beat.
  Ids from the clip counter (`normalizeIds`). Saved as `[[markers]]` (beat, name); part of undo
  ("Add marker", "Move marker", "Rename marker", "Delete marker"); listed with the project's own
  fields in the history, the store's restore plan and the saved-state checks.
- The **Markers lane**, first of the arrangement's global lanes (above Signature and Tempo, shown
  and hidden with them). A flag per marker with its name. Click a marker to put the playhead
  there; drag it to move it (snapped unless Shift); double-click it to rename it; double-click
  empty space to add one there (named "Marker N", its name then edited); right-click for its menu
  (rename, loop this section, move the playhead here, delete). With the global lanes hidden, the
  ruler shows the markers' flags and they still click to locate.
- **Navigation**, each a keymap action in the Transport menu: Add marker at the playhead (Ctrl+M),
  Previous / Next marker (Ctrl+Alt+Left / Ctrl+Alt+Right), Go to marker 1-9 (Alt+1 to Alt+9), Loop this
  section (the markers either side of the playhead).
- **Markers list** (Transport > Markers..., a dialog): every marker with its position, its name
  to edit in place, Go and Delete; Add at the playhead.
- The transport bar shows the section the playhead is in (the marker before it).
- Locators: the left and right locators are the loop's start and end (as now, set on the ruler);
  "Loop this section" sets them from the markers.

## MIDI import

- `daw/io/MidiImport.h/.cpp`: `readMidiFile(bytes) -> MidiFile` for Standard MIDI Files, type 0
  and 1: the header's ticks per quarter (SMPTE time division refused), every track's events with
  running status, note on/off (velocity 0 as off, overlapping notes on one key paired first-in
  first-out), controllers, the pitch bend, program changes, and the meta events for names, tempo
  and time signatures. Positions are beats (ticks / ticks per quarter), so the tempo doesn't move
  them. A file that runs short or lies about its lengths is refused with a reason, never read past.
- `importMidi(project, file, at, track)`: the file's parts (each track with notes; a type 0 file is
  split by channel) as clips starting at beat `at`, on the instrument track `track` when one was
  aimed at (the first part), the rest on new instrument tracks below it named after them. Notes
  keep their velocities and lengths; controllers and the bend become the clip's controller lanes.
  New tracks play the built-in synth, so they sound at once.
- Tempo and signatures: when the file's differ from the project's, a dialog asks whether to use the
  file's (placed from the import's beat) or keep the project's; undo reverses either.
- Project > Import MIDI... opens a file dialog; a drop does the same at the place it lands.

## File drops

- `engine::Window` asks GLFW for dropped files (`glfwSetDropCallback`) and queues them with where
  they landed (`takeDrops()`); DawApp handles them each frame.
- By kind (extension): audio files (what the pool reads), MIDI files (`.mid`, `.midi`), a project
  (`.vbp`, opened as Project > Open would, asking about unsaved changes). Anything else: a message
  saying what can be dropped.
- Where: on an arrangement lane, at the beat under the drop (snapped to the grid) on that track
  when it takes that kind (audio on an audio track, MIDI on an instrument track), else on new
  tracks; below the tracks, new tracks; anywhere else in the window, at the playhead on new tracks.
  Several audio files go one after another on one track, as Import audio places them.
- Windows' simple drop (WM_DROPFILES) tells where a drop landed but not where a drag is while it
  is held, so there is no highlight during the drag; the arrangement flashes the row it landed on.

## As built

- The store's `normalizeIds` looks again after `editMarkers()` (it only looked after edits that
  add tracks or clips), so a marker added has its id at once.
- Ctrl+M adds a marker without asking for its name (it is for marking places while the song
  plays); adding one in the lane or a menu asks.
- Drops are handled one a frame, so what a drop adds is laid out before the next is placed.
- The drop's place on the arrangement comes from the cursor GLFW reports with the drop; there is
  no highlight while dragging (Windows' simple drop doesn't say), only the outline where it landed.
- `Window::queueDrop` feeds the same queue as a real drop, for the `drop` demo; a real drag from
  Explorer couldn't be made here, so the GLFW callback itself was checked only by building it.
- With the global lanes hidden, a marker's tab on the ruler can cover a bar number.

## Stages

1. Markers: model, file, undo, tests.
2. Markers: the lane, the ruler flags, navigation actions, the list, the transport bar.
3. MIDI import: the reader and `importMidi`, with tests built from `writeMidiFile` and hand-made bytes.
4. File drops: the window, DawApp's handling, the arrangement's landing flash; Import MIDI in the menu.
5. Docs, render scenes, demos.
