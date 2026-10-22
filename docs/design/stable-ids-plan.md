# Stable IDs: plan

## Problem

Tracks and clips have no identity: they are their index. So the audio side can't tell "track 3
was deleted" from "tracks 3 onwards changed", and `DawApp::restore` (undo/redo) drops every track's
processor whenever tracks came or went (`bridge_->dropAll()`): every plugin host restarts, reverb
tails cut, instruments are handed over again. Views are rebuilt by hand from 8 call sites, and
anything that must outlive a frame (an open device view, arming kept across undo) holds an index
that shifts.

## Design

- **Identity**: `Track::id` and `Clip::id` (`ObjectId`, `uint32_t`, 0 = none), unique in the
  project (clip ids across the whole project), kept through edits, undo, saving and opening.
- **Normalization, not discipline**: code that makes tracks and clips (dozens of places, tests,
  recording, imports) doesn't have to know about ids. `Project::normalizeIds(next)` gives every 0 and
  every duplicate after the first a fresh id. `ProjectStore::normalizeIds()` runs it with a session
  high-water mark (an id is never reused), on construction, on `replace`, at the start of every frame
  (DawApp), and at the start of `AudioBridge::sync`. When it changes something it moves `revision()`
  and those tracks' revisions, so the store's verifier stays quiet.
- **Ids are bookkeeping**: saved (`id = N` on `[[tracks]]` and `[[tracks.clips]]`, written when
  non-zero, optional when read, so older files open and get ids), but ignored by `sameContent`,
  `sameSaved`, `describeChange` and the audio hashes, so normalizing is never an undo step or an
  unsaved change.
- **Reconciliation** (React's keyed lists, an ECS's entity handles): `AudioBridge::sync` matches
  `tracks_` to the project's tracks by id and kind. A kept track keeps its processor, players and
  device chain wherever it moved; a vanished one is removed from the graph; a new one is made.
  `tracks_` ends in project order, so the index queries keep working. `removeTrack` goes away;
  `dropAll` stays only for a project opened or made.
- **Views observe**: the arrangement and the mixer rebuild themselves when `structureRevision()`
  moves, instead of 8 hand-placed `rebuild()` calls.

## Waves (Haiku 5.5 agents; one incremental part each, disjoint files)

| Wave | Agent | Owns | Done when |
|---|---|---|---|
| 0 | orchestrator | `Project.h`, `ProjectStore.h`, `AudioBridge.h` contracts | headers compile |
| 1 | A model | `Project.cpp`, `ProjectStore.cpp` | `normalizeIds`, `trackIndex`, `findTrack`; the store's normalize and high-water mark |
| 1 | B file | `ProjectFile.cpp` | ids written and read, normalized on load |
| 1 | C tests | `tests/ProjectSelfTest.cpp` | id checks (normalize, store, file, compare) |
| 2 | D bridge | `AudioBridge.cpp` | `reconcile`, `makeTrack`, `removeAudio`, counters; `removeTrack` gone |
| 2 | E app | `DawApp.cpp`, `DawAudio.cpp`, `RenderJob.cpp` (if needed) | undo without `dropAll`, arming by id, normalize each frame, no `removeTrack` |
| 2 | F bridge tests | `tests/BridgeSelfTest.h/.cpp`, one call in `ProjectSelfTest.cpp` | processors kept across delete, undo, reorder, kind change |
| 3 | G views | `Arrangement.*`, `Mixer.*`, `DawApp.cpp`, `DawAudio.cpp` | views rebuild on `structureRevision`; manual `rebuild()` calls gone |
| 4 | H device views | `DawPlugins.cpp`, `DawApp.h/.cpp` | open device views keyed by track id |
| 4 | I docs | `CLAUDE.md` | the ids, normalization, reconciliation and observation documented |

Between waves the orchestrator builds, runs every self-test, reviews and commits.

## Out of scope (follow-ups this enables)

Device ids (effects moved left or right keep their state), clip selection by id, reordering tracks
by drag, automation and send targets, the routing graph (pitch 3).
