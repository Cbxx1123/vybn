# What was left "Not done": plan

Status: built, with the differences under "As built". The items the docs listed as not done: the VST3
ones at the end of `vst3-plan.md`, and the tests' under "Not done, and why" in `testing.md`. Each is done
here, or kept with its reason.

## VST3

All in the host (`plugin/host/Vst3Format.cpp`) but the last; the protocol doesn't change.

1. **Units** (`IUnitInfo`): a parameter's unit, and the units above it, become its `module`
   ("Filter/Envelope"), as a CLAP plugin's module path is. The root unit adds nothing.
2. **Program lists**: a parameter flagged `kIsProgramChange` in a unit with a program list is a list
   (stepped, an enum) of the programs, its steps from the list when the plugin gives none, and its texts
   the programs' names. Changing it is changing any parameter.
3. **`IComponentHandler2`**: `setDirty` is the same news as CLAP's state-dirty (the project is marked
   changed); group edits are accepted (the edits inside them arrive one by one, as now);
   `requestOpenEditor` is refused, since the app decides when an editor opens.
4. **64-bit processing**: a plugin that only processes doubles gets them: the host keeps double buffers
   for every bus, made in `activate`, and converts the block's audio in and out around `process`.
5. **Note ids**: each note gets an id (its key and channel), on its note-on and its note-off, so a
   plugin can tell its voices apart. **Note expression stays not done**: the app has nothing per note to
   send (a note is a key, a velocity and a length), so it needs per-note data in the model, the file, the
   piano roll and the bridge first: a feature of its own.
6. **`moduleinfo.json`**: a bundle's `Contents/Resources/moduleinfo.json` lists its classes, so the app
   reads it and lists the plugins without starting a host or loading the module. A bundle without one, or
   with one that can't be read or lists no plugin, is scanned as now. The sidechain is still learnt by a
   probe. The file is JSON5 as Steinberg writes it (comments, trailing commas), read by a small parser of
   its own in `plugin/`, which can't use the DAW's.

Tests: the test bundle gets units (VST Sine's parameters in "Voice" and "Voice/Pitch"), a program list
(a "Program" parameter on VST Sine with three named programs), `setDirty` (VST Gain calls it when its
editor is clicked), a 64-bit-only plugin (VST Duck processes doubles alone) and a `moduleinfo.json`
written by the build. The suite scans a made-up bundle whose module isn't a DLL at all, which only
the JSON can list.

## Tests

7. **Splitting the export suite**: `--export-selftest` takes `--group render` (the render checks) or
   `--group rest` (everything else); with neither it runs both, as now. `tools/test.sh` runs them as two
   suites, `export-render` and `export`.
8. **Coverage**: a `VYBN_COVERAGE` option builds with `--coverage` into its own tree and output folder
   (`VYBN_OUTPUT_DIR` becomes a cache variable), and `tools/coverage.sh` configures and builds it, runs
   the suites against it and prints each source file's line coverage from gcov, lowest first.
9. **The UI drag**: the track reorder drag is checked already (`foldDragChecks` drags headers, and the
   render suite hands input to its views), so that line was stale. The browser's instrument drag isn't: a
   check opens the browser over an arrangement, presses on an instrument's row and moves past the threshold
   (the drag starts, with the row's key; a click alone doesn't start one), and the arrangement's
   `trackAt` gives the drop rows (a track, the space under the tracks). The glue between them is
   `DawApp`'s and stays unchecked.

## As built

- VST3 items 1 to 6 as planned. The plugin suite has 132 checks (seven new), about 4 s. Two of them
  were seen to fail on a broken host (note ids of -1, modules left empty) before they passed.
- Item 7 was not done: measured first, the export suite now takes 5-6 s (it took 15.6 s when the note
  was written), beside render (5.5 s) and engine (5.2 s), and the wall is about 10 s, most of it the
  plugin suite running alone at the end. Splitting it would save under half a second. `testing.md`
  says so.
- Item 8 as planned: `tools/coverage.sh`, about 50 s once built; the suites all pass in the
  instrumented build, and it measured 56.7% of 39,878 lines.
- Item 9: `browserDragChecks` in the render suite, with `AssetBrowser::rowRect(key)` to find a row. A
  mutation (a drag started again on every move) turned its "once" check red.
