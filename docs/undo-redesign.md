# Undo redesign

## Baseline (before)

Measured before any change to History's algorithm. The only code change is the move of DawApp::restore's
state preparation into `ProjectSession::restoreState` (behaviour identical). Benchmark: the `project`
suite, block "Undo" in `tests/ProjectSelfTest.cpp`.

Project: 32 instrument tracks x 20 clips x 64 notes (640 clips, 40,960 notes). Machine: 12 cores,
Windows 10, TDM-GCC 64-bit. Each number is a typical value of 2-3 runs (they vary by about 15%).

| Measure | Debug | Release |
|---|---|---|
| drag frame, edit on the last track (`recordHistory(settled=false)`, mean of 200) | 0.38 ms | 0.08 ms |
| drag frame, edit on the first track | 0.0005 ms | 0.0001 ms |
| settled frame after the drag | 2.5 ms | 1.0 ms |
| selection change (`recordHistory` after `store.select`, mean of 200) | 0.6 ms | 0.12 ms |
| undo (`history().undo` + copy + `restoreState`, mean of 50) | 3.9 ms | 0.8 ms |
| redo (same, mean of 50) | 3.9 ms | 0.55 ms |
| first frame after an undo (`recordHistory`) | 0.8 ms | 0.12 ms |
| churn after one undo of a single-note edit on one track | trackRevision 32/32, mixRevision 32/32, structure moved, master moved, selection moved | same |
| `storedClips()` after 50 single-note edits on one clip | 691 | 691 |
| bytes held, estimated | ~1.9 MB | ~1.9 MB |

Machine-readable form (what the bench prints):

```
undo.debug   drag_ms=0.38 drag_first_track_ms=0.0005 settled_ms=2.5 select_ms=0.6 undo_ms=3.9 redo_ms=3.9 after_undo_frame_ms=0.8
undo.release drag_ms=0.08 drag_first_track_ms=0.0001 settled_ms=1.0 select_ms=0.12 undo_ms=0.8 redo_ms=0.55 after_undo_frame_ms=0.12
undo.churn   tracks=32/32 mix=32/32 structure=1 master=1 selection=1
undo.memory  stored_clips=691 bytes=1945176
```

Notes on reading them:

- The drag frame depends on where the edit is. `sameContent` stops at the first difference, so an edit on
  the first track is nearly free and one on the last track pays for comparing every track before it. The
  table's main row is the worst case.
- Memory is `storedClips() * (sizeof(Clip) + 64 * sizeof(Note))` plus, for each step, one shell
  (`sizeof(Project) + 32 * sizeof(Track)`). It leaves out heap strings and the controller lanes (none here).
  The 691 is the 640 clips of the first step, kept alive by every step that shares them, plus one new clip
  per edit step (51 here: two drag runs plus 50 edits).
- Churn is the whole point of the redesign: undoing a one-note edit on one track moves every track's
  revisions, so the audio bridge and every view resync all 32 tracks.

## Reproduce

```
cmake --build build -j 8
VIBIN_TEST_BENCH=1 output/main.exe --project-selftest | grep "INFO undo"      # Debug
cmake -S . -B build-release -G "MinGW Makefiles" -DCMAKE_C_COMPILER=C:/TDM-GCC-64/bin/x86_64-w64-mingw32-gcc.exe -DCMAKE_CXX_COMPILER=C:/TDM-GCC-64/bin/x86_64-w64-mingw32-g++.exe -DCMAKE_BUILD_TYPE=Release
cmake --build build-release -j 8       # writes output/main.exe too: rebuild build afterwards (or copy the exe aside)
VIBIN_TEST_BENCH=1 output/main.exe --project-selftest | grep "INFO undo"      # Release
```

The correctness checks ("Undo: ..." in the project suite, fast tier) must stay true after the redesign:
a single-note edit is put back exactly, redo applies it again, and arming, monitoring and the loop are kept
across `restoreState`. The churn numbers are reported only, never asserted.

## After chunk 2 (History follows the store's revisions)

`History::update(const ProjectStore &, ...)` compares only the tracks whose `trackRevision` or
`mixRevision` moved since the last state recorded (all of them when the structure moved), and the last state
recorded is held as pointers (project fields, a shared shell and shared clips per track), so a selection
change or a pending edit never copies the project. Once a difference is seen on an unsettled frame, nothing
more is compared until the settled frame. Undo is still applied with `store.replace` (chunk 3), so the churn
is unchanged. Same project (32 x 20 x 64) and method as the baseline; typical values of 3 runs.

| Measure | Debug before | Debug after | Release before | Release after |
|---|---|---|---|---|
| drag frame, edit on the last track | 0.38 ms | 0.0001 ms | 0.08 ms | 0.0001 ms |
| drag frame, edit on the first track | 0.0005 ms | 0.0001 ms | 0.0001 ms | 0.00005 ms |
| settled frame after the drag | 2.5 ms | 1.3 ms | 1.0 ms | 0.5 ms |
| selection change | 0.6 ms | 0.002 ms | 0.12 ms | 0.0005 ms |
| undo | 3.9 ms | 3.0 ms | 0.8 ms | 0.45 ms |
| redo | 3.9 ms | 3.0 ms | 0.55 ms | 0.45 ms |
| first frame after an undo | 0.8 ms | 1.0 ms | 0.12 ms | 0.2 ms |
| churn (tracks / mix / structure / master / selection) | 32/32, 32/32, 1, 1, 1 | same | same | same |
| `storedClips()` after 50 edits | 691 | 691 | 691 | 691 |
| bytes held | ~1.95 MB (old estimate) | ~2.17 MB (`storedBytes()`) | same | same |

```
undo.after2.debug   drag_ms=0.0001 drag_first_track_ms=0.0001 settled_ms=1.3 select_ms=0.002 undo_ms=3.0 redo_ms=3.0 after_undo_frame_ms=1.0
undo.after2.release drag_ms=0.0001 drag_first_track_ms=0.00005 settled_ms=0.5 select_ms=0.0005 undo_ms=0.45 redo_ms=0.45 after_undo_frame_ms=0.2
undo.after2.memory  stored_clips=691 bytes=2170528 (old estimate formula: 1945176)
```

- The bytes figure now comes from `History::storedBytes()`, which also counts the pointer tables each step
  holds (640 clip pointers and 32 track entries a step, about 11 KB); the old estimate left them out, so the
  two aren't comparable. With the old formula the figure is unchanged, since the stored clips are the same.
- What remains in the settled frame is `describeChange`, which needs the previous state as a `Project`, so it
  copies it whole (about 1 ms Debug), plus copying the pointer tables into the step. The frame after an undo
  is slower because the history has lost the revisions it went by and compares everything once; chunk 3
  (applying undo by revisions) removes it. Undo and redo still copy the project and `replace` it.

## After chunk 3 (undo and redo applied by what differs)

`History::undo/redo(store, ...)` return a `HistoryRestore` plan (the fields that differ and the tracks whose
shell or clip pointers differ, confirmed by comparing) and `ProjectStore::restore` applies it through
`editTrack`, `editMix`, `editMasterEffects`, `setTempo`, the new `editSettings` (name, beats per bar, length:
moves `revision()` only), `editExportPresets`/`editExportMetadata` and `select`. A plan that adds, removes,
moves or re-kinds tracks is `structural` and replaces the project as before. The history then adopts the
store's revisions, so the next frame compares nothing. The label of a settled step is now found from the
changed tracks alone. Same project (32 x 20 x 64), typical of 3 runs; "before" is the baseline at the top.

| Measure | Debug before | Debug after | Release before | Release after |
|---|---|---|---|---|
| drag frame, edit on the last track | 0.38 ms | 0.0001 ms | 0.08 ms | 0.0001 ms |
| drag frame, edit on the first track | 0.0005 ms | 0.0001 ms | 0.0001 ms | 0.00004 ms |
| settled frame after the drag | 2.5 ms | 0.11 ms | 1.0 ms | 0.02 ms |
| selection change | 0.6 ms | 0.002 ms | 0.12 ms | 0.0005 ms |
| undo | 3.9 ms | 0.065 ms | 0.8 ms | 0.009 ms |
| redo | 3.9 ms | 0.06 ms | 0.55 ms | 0.008 ms |
| first frame after an undo | 0.8 ms | 0.000 ms | 0.12 ms | 0.000 ms |
| tracks whose trackRevision moved by one undo | 32/32 | 1/32 | 32/32 | 1/32 |
| tracks whose mixRevision moved | 32/32 | 0/32 | 32/32 | 0/32 |
| structure / master / selection revision moved | 1 / 1 / 1 | 0 / 0 / 0 | 1 / 1 / 1 | 0 / 0 / 0 |
| `storedClips()` after 50 edits | 691 | 691 | 691 | 691 |
| bytes held (`storedBytes()`; the old estimate gave 1.95 MB) | ~2.17 MB | ~2.17 MB | ~2.17 MB | ~2.17 MB |

```
undo.after3.debug   drag_ms=0.0001 drag_first_track_ms=0.0001 settled_ms=0.11 select_ms=0.002 undo_ms=0.065 redo_ms=0.06 after_undo_frame_ms=0.000
undo.after3.release drag_ms=0.0001 drag_first_track_ms=0.00004 settled_ms=0.02 select_ms=0.0005 undo_ms=0.009 redo_ms=0.008 after_undo_frame_ms=0.000
undo.after3.churn   tracks=1/32 mix=0/32 structure=0 master=0 selection=0
```

The undo and redo times include the store's edit of the one track (copying its 20 clips back); they no
longer copy the project. The bench's churn edit leaves the selection as it was, so its selection revision
does not move; an undo that changes the selection moves `selectionRevision()` through `select`.
