# Step sequencer: plan

A drum machine's grid as the editor's second page for instrument clips, beside the piano roll.
Rows are keys (the General MIDI kit, then any other key the clip uses, and rows added by hand),
columns are steps. A step is on while a note of its key starts inside it, so **nothing new is
saved**: the piano roll, undo, autosave, playback, MIDI export and audio export all see ordinary
notes. No audio-thread code changes.

## Contracts (written first, fixed for the swarm)

| File | What |
|---|---|
| `src/daw/StepSequence.h` | Plain logic on `std::vector<Note>`: the grid, steps on/off, velocities, rows as patterns, shift, swing, mute, Euclidean/every/random patterns, the GM kit and drum names |
| `src/daw/StepSequencer.h` | `StepSequencerView`, the `ui::Component`: grid, row names, velocity lane, painting, scrolling, playhead; its `ViewSettings` (step size, swing, gate, velocity, hearing) as a `key=value` line |
| `src/daw/StepMenus.h` | The row menu, add-row menu, step-size menu and the Euclidean dialog |

`CMakeLists.txt` already lists `StepSequence.cpp`, `StepSequencer.cpp`, `StepMenus.cpp` and
`tests/StepSelfTest.cpp`. `tools/check.sh FILE...` syntax-checks files with the build's flags.

## Waves (Haiku 5.5 agents, one incremental part each)

| Wave | Agent | Owns (only these files) | Done when |
|---|---|---|---|
| 1 | A: logic | `src/daw/StepSequence.cpp` | every function in the header, `tools/check.sh` OK |
| 1 | B: tests | `tests/StepSelfTest.h/.cpp`, `tests/NoteSelfTest.cpp` (one call) | checks for every function, run from `--notes-selftest` |
| 1 | C: view | `src/daw/StepSequencer.cpp` | the view as documented in its header, `tools/check.sh` OK |
| 2 | D: menus | `src/daw/StepMenus.cpp` | the menus and dialog as documented, `tools/check.sh` OK |
| 2 | E: app | `DawApp.h/.cpp`, `Settings.h/.cpp`, `Actions.cpp`, `src/main.cpp` | Editor page 2, Piano roll/Steps switch, toolbar, persistence, `view.steps` action, `--demo steps` |
| 3 | F: docs | `CLAUDE.md` | the new files, the editor's page, the demo and the self-test documented |

Agents never run `cmake --build` (one shared build tree). Between waves the orchestrator builds,
runs every self-test, reviews the diff and commits.
