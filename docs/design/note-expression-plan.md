# Per-note expression: plan

Status: agreed, not yet built. Decided with the user: **tuning only** for now (the others are left
for later, and the data is shaped so they add without changing the file); curves on the note; edited
in the piano roll's lane; sent to both VST3 and CLAP plugins; the built-in instruments ignore it.

## Problem

A note is a key, a velocity, a length and a muted flag (`daw::Note`). Everything that changes while it
sounds is a clip's controller lane (`ControlLane`), which reaches every note on the track at once: a
bend bends the whole chord. Plugins can do more. VST3 has note expression (`kNoteExpressionValueEvent`
against a note id, and `INoteExpressionController` to say which types a plugin takes) and polyphonic
pressure (`kPolyPressureEvent`); CLAP has `CLAP_EVENT_NOTE_EXPRESSION` (volume, pan, tuning, vibrato,
expression, brightness, pressure) against a key, channel and note id. The host already gives each
VST3 note an id, its channel and key (`channel * 128 + key`), on its note-on and note-off, so the
ids are there; what is missing is anything per note to send.

## Terms

- An **expression**: one kind of change a single note can carry while it sounds (its tuning, its
  pressure, ...).
- A note's **expression curve**: the expression's values over the note's length, in beats from the
  note's start. A note without a curve for an expression has that expression's neutral value.

## Which expressions

Tuning, now. The table is the set this is shaped for: the three MPE has (the ones a controller
such as a Seaboard or a Linnstrument plays) and the two both formats name. Only the first row is
built; the rest add a word to the file, a row to the lane's menu and a conversion in each host.

| Expression | Stored as | Neutral | VST3 | CLAP |
|---|---|---|---|---|
| Tuning | semitones, -48 to +48 | 0 | `kTuningTypeID` (0.5 + st / 240) | `TUNING` (semitones) |
| Pressure | 0-1 | 0 | `kPolyPressureEvent` | `PRESSURE` |
| Timbre | 0-1 | 0.5 | `kBrightnessTypeID` | `BRIGHTNESS` |
| Volume | gain 0-4 (1 is unity) | 1 | `kVolumeTypeID` (0.25 is unity: `gain / 4`) | `VOLUME` |
| Pan | 0-1 (0.5 centre) | 0.5 | `kPanTypeID` | `PAN` |

Left out, unless asked for: vibrato and "expression" (both formats have them, but few plugins do
anything with them and a drawn curve can do what vibrato does), VST3's text and phonemes, a plugin's
own custom types (`kCustomStart` and up, which would need the plugin loaded to name them).

The values are stored in their own units (semitones, a gain), not 0-1, so the file reads plainly and
the formats' conversions live in the host, once each.

## On a note

Curves, as controller lanes are.

```cpp
enum class Expression : uint8_t { Tuning }; // Pressure, Timbre, Volume, Pan later
struct NoteCurve {
    Expression type = Expression::Tuning;
    std::vector<ControlPoint> points; // beats from the note's start, sorted; in the expression's units
    float valueAt(double beat) const; // straight lines; before the first point its value
};
struct Note {
    ... // as now
    std::vector<NoteCurve> expression; // at most one per type; empty for almost every note
};
```

- One point is a fixed value for the whole note, so "a few values" is a curve of one point: the
  curve covers both and costs an empty vector on a note without any.
- The other way, a value per expression on the note (five floats, no curves), is smaller and simpler
  to edit, but can't bend into a note or swell its pressure, which is most of what note expression is
  for.
- Points past the note's end are kept (shortening a note and lengthening it again gives them back)
  but never play. Moving, copying, quantising and transposing a note carry its curves untouched,
  since they are relative to its start.
- Splitting a note (the Slice tool, takes' `compRange` cutting a note at a range edge) splits its
  curves: each part gets the points in its span, shifted to its new start, with a point at the cut
  holding the value there so nothing jumps.
- Joining (if a "glue" exists or is added) keeps the first note's curve and appends the second's,
  shifted.

## History and the model's rules

- `sameNote` in `History.cpp` compares the curves; `describeNotes` names an edit of them ("Note
  tuning"). The notes are inside clips, which are inside a track's content, so
  `Track`'s size doesn't change and its `static_assert` doesn't move; `sizeof(Note)` grows by a
  vector, which the history's byte count (`sizeof(Note)` per note) follows by itself, plus the
  points' bytes added there.
- `ProjectStore::restore` restores clips whole, so it needs nothing. `VYBN_VERIFY_STORE` checks it.

## The file

A note with curves gets an `expression` key, an array like a lane's points; a note without has no
key, so files with no expression are byte for byte as now, and older files load unchanged.

```toml
notes = [
  { start = 0, length = 1, key = 60, velocity = 0.8 },
  { start = 1, length = 2, key = 64, velocity = 0.7, expression = [
      { type = "tuning", points = [ { beat = 0, value = 0 }, { beat = 0.5, value = 2 } ] }
      ] },
]
```

- `type` is a word ("tuning", "pressure", "timbre", "volume", "pan"), not a number, so the saved
  form never depends on the enum's order. An unknown word is skipped with a warning (a newer file),
  not an error.
- Values are clamped to the type's range, points sorted, one curve per type (a second is dropped),
  as controller lanes are read.

## The piano roll

In the lane under the grid, as the velocity and controllers are, so nothing new is
learnt.

- The lane's menu gets **Note tuning** under the controllers (and later the other expressions).
- In one of them each note is drawn as its curve across its own span (the neutral value as a faint
  line where it has none), the selected notes bright, the rest faint; a chord's curves overlap, so the
  selected ones are drawn last.
- Editing, the same gestures as a controller lane, but onto notes:
  - Drag across: draws the curve of each note the stroke passes over, at the stroke's height. With
    notes selected only the selected ones are drawn on (the way to draw on one note of a chord);
    without, every note under the stroke.
  - Alt-drag: a straight line, the same rule for which notes.
  - A handle at each note's start: drag it to set the note's value for its whole length (the curve
    becomes one point), the selection following by as much, as velocity handles do.
  - Erase tool, or Delete over the lane: the curves of the notes under the stroke (or the selected
    notes) for this expression go.
  - The lane's "clear" (the menu's remove) clears this expression from the selected notes, or all.
- Tuning is also shown in the grid: a thin line along the note, offset by its curve (a semitone a
  row), so a bend can be seen without the lane open. Read only; editing stays in the lane.
- The header's readout shows the value under the mouse in semitones ("+2.0 st").

Another way, not recommended for now: drawing directly on a note in the grid (dragging a note's end
up or down to bend it, as some DAWs do). It is quicker for glides but needs its own gestures beside
moving and resizing notes; it could come later on top of the same data.

## Playback

- `audio::SequencedNote` gets its curves (a pointer to them in the `NoteSequence`, which owns a flat
  copy made on the main thread when the sequence is built, as controller curves are).
- `NotePlayer` sends, for each sounding note that has curves: each curve's value at its note-on (on the
  same frame, just after it), then the values as they change while it sounds, read every 64 frames of
  the block (about 1.5 ms at 44.1 kHz, fine enough for a bend without a stream of events for a flat
  curve: a value is sent only when it moved).
- `Instrument` gets `noteExpressionAt(int key, Expression type, float value, uint32_t frame)`, empty by
  default (the built-in synth and sampler ignore it, unless chosen below); `ChainInstrument` passes it
  on; `PluginInstrument` turns it into a plugin event.
- Two sounding notes of the same key on one track share an id (the host's ids are the key and
  channel), so their expressions would mix. Rare in a piano roll; the newer note wins, and the design
  says so rather than adding a channel allocator.
- Live MIDI (an MPE keyboard) and recording expression from it: not in this plan (see the end).

## The protocol and the hosts

- `plugin::EventKind::NoteExpression`: `id` the key, `channel`, `value` in the expression's units, and
  the type in a new `uint32_t expression` field of `Event` (which grows from 24 bytes to 32, 4 KB more
  of shared memory for a block's 512 events). `kProtocolVersion` goes to 5.
- **VST3** (`host/Vst3Format.cpp`):
  - On load, `INoteExpressionController` (queried from the controller): the types it lists on the
    event bus's channel 0 (`getNoteExpressionCount`, `getNoteExpressionInfo`), kept as a set of the
    standard types it takes. Pressure is sent as `kPolyPressureEvent`, which needs no listing.
  - A `NoteExpression` event becomes a `kNoteExpressionValueEvent` with the note's id (`channel * 128 +
    key`), the type id and the value normalised as in the table; a type the plugin didn't list is
    dropped.
- **CLAP** (`host/ClapFormat.cpp`): a `clap_event_note_expression` with the
  key, channel, the note id the host gives CLAP notes (the same `channel * 128 + key`, set on note-on
  and note-off as VST3's are, where today it sends -1), and the value in CLAP's units (tuning in
  semitones, volume as a gain: the stored units already). CLAP has no query; a plugin ignores what it
  doesn't take.

## Built-in instruments

Not in this plan. The synth and sampler would need per-voice tuning, gain, pan and a
pressure target in their voices and presets (what pressure and timbre do to a patch is a sound
design choice of its own). The data and the bridge are ready for them, and it can be a short plan of
its own.

## Tests

- project suite: a note's curves through the file (written only when present, read back equal, an
  unknown type skipped, values clamped, an old file loading unchanged), through undo (an edit of a
  curve is one step, named), splitting a note and `compRange` cutting one (the curves split, no jump
  at the cut), copying and moving carrying them.
- audio suite: `NotePlayer` sending a note's values at its note-on and as its curve changes, nothing
  for a flat curve, nothing for a note without curves.
- plugin suite: VST Sine (`tests/plugins/TestVst3.cpp`) lists tuning and volume in an
  `INoteExpressionController` and plays a note's tuning (its pitch) and volume; checks in
  `tests/Vst3SelfTest.cpp` that a tuning of +12 doubles the sine's frequency on that note alone (two
  notes held, one tuned), and that a plugin that lists no tuning never gets it. The
  CLAP test instrument gets the same tuning check through `CLAP_EVENT_NOTE_EXPRESSION`.
- render suite: a piano roll scene with the tuning lane open over a chord with one bent note.

## Stages

1. Model, history, file, splitting; their tests.
2. The bridge, `Instrument`, the protocol (version 5), both hosts, the test plugins; their tests.
3. The piano roll: the lane's expressions, drawing and editing, the tuning line in the grid; a render
   scene.
4. Docs: `architecture/daw.md` (the note, the file), `architecture/plugin.md` (the event, the hosts),
   `audio.md` (`NotePlayer`), the end of `vst3-plan.md`.

## Not in this plan

- Live MPE input (a keyboard's per-channel bend, pressure and CC 74 played and recorded into notes'
  curves): it needs MPE zones in the MIDI input and a channel per note, and is the natural next step.
- MIDI import and export of expression (MPE files, or poly aftertouch for pressure).
- Pressure, timbre, volume and pan (the rest of the table above); a plugin's custom expression
  types, vibrato and VST3's "expression" type.
- The built-in instruments.
