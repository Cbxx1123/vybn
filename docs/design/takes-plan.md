# Take lanes and comping: plan

Status: implemented (stages 1-4), with the differences under "As built". Decided with the user
before starting: recording over material makes takes (it keeps an "Original" lane); the newest
take on top; comping by swipe, click and double-click; audio and MIDI takes alike.

## Problem

A recording makes one clip in the track's lane (`RecordingSession`). Recording over a clip lays a
new one on top (overlapping clips play together), and the loop is switched off while recording, so
there is no way to record several passes, keep them, and build the best performance from parts of
each.

## Terms

- A **take**: one recorded pass, kept as a clip in a **take lane** under its track. Take lanes
  don't play.
- The **comp**: what the track plays, made of **comp sections** in the track's own lane (its
  `clips`), each cut from a take. A comp section is an ordinary clip that remembers its take.
- **Comping**: choosing, range by range, which take the comp plays.

## Model

- `TakeLane { ObjectId id; std::string name; std::vector<Clip> clips; }` and `Track::takeLanes`,
  newest first. Lane ids come from the same counter as clip ids (`normalizeIds` covers lanes and
  their clips).
- `Clip::take` (`ObjectId`, `kNoId` for an ordinary clip): the take clip a comp section was cut
  from. Everything downstream of the comp (playback, export, the clip editors, splitting, moving)
  sees ordinary clips: nothing there changes. An audio section is the take's file at an offset; a
  MIDI section holds the take's notes and controller points in its range.
- `daw/model/Takes.h/.cpp`, the rules, on a `Track` and the `TempoMap`:
  - `compRange(track, from, to, takeId)`: the comp plays `take` over [from, to): what was there
    is cut away (sections trimmed, split or removed, as clips are), a section cut from the take
    goes in, and neighbouring sections of the same take that join up are merged.
  - `compSection(track, beat)`: the section around a beat; `useTake(track, laneId)` the whole
    take where it has material; `newestComp(track, from, to)` each point from the newest take that
    covers it (what recording leaves).
  - `moveBoundary(track, beat, to)`: the edge between two sections moved, both following.
  - `flatten(track)` (take lanes gone, the comp stays), `deleteUnused(track)`, `removeLane`.
  - `takeUse(track, lane)`: the ranges of a lane's takes the comp plays, for drawing.
- History: `takeLanes` is part of a track's content (compared, copied, restored); undo names
  "Comp", "Delete take", "Flatten comp", "Record takes". `Track`'s size assert moves.
- File: `[[tracks.take_lanes]]` (id, name) with `[[tracks.take_lanes.clips]]` in the clip format,
  and `take = id` on a comp section. Older files have none.
- Auditioning a lane (hearing a take instead of the comp) is a view state on the track, like
  monitoring: not saved, not undone.

## Recording

- The loop stays on while recording: each lap is a pass. The transport counts laps (`laps()`,
  bumped where `advance` wraps; `TransportState::lap` stamps MIDI with it).
- Audio: the input recorder already captures one continuous stream. A lap's take is that one file
  at an offset (lap boundaries follow from the start position and the loop's samples), so a long
  loop recording is one WAV and many takes. While recording, the current lap grows in the track's
  lane as now; at each wrap the lap that ended becomes a take in a new lane and the growing clip
  starts again at the loop's start.
- MIDI: the same, by the lap the messages are stamped with; notes held over the wrap end at the
  loop's end.
- When recording stops (`foldTakes`): a single pass over empty space stays a plain clip, as now.
  Otherwise every pass is a take (a partial last lap too), material that was in the range before
  (ordinary clips) moves into a lane of its own ("Original") so nothing is lost, and the comp over
  the range is `newestComp`: the last take, with earlier ones filling where it doesn't reach. The
  track's take lanes open.

## Playback

- The comp is the track's clips, so the bridge plays it unchanged. At an internal boundary between
  two audio sections the bridge overlaps them by a short crossfade (10 ms, equal power, through
  `ClipSequence::crossfade`) so edits never click; the model keeps sections butted.
- Auditioning a lane: the bridge plays that lane's clips (audio or notes) instead of the comp.

## Arrangement

- A **Takes** toggle in the track header's radial menu (T, with M, S, R, A, W) shows the lanes
  under the track row, above its automation lanes. Recording that leaves takes opens them.
- A take lane: its header (name, a listen toggle, "use this take", delete) and its takes, drawn
  like clips but faint, with the ranges the comp plays from them bright and outlined. The comp's
  boundaries run down through the lanes.
- Comping, all snapped to the arrangement's grid unless Shift:
  - **Swipe**: drag across a take: that range of the comp now comes from it, live as you drag.
  - **Click**: the comp section under the mouse switches to this take.
  - **Double-click**: the whole take.
  - Drag a **boundary** (in the lanes) to move the edge between two sections.
  - Hover shows what a click would choose.
- The comp's sections in the track's own lane carry a small badge with their take's number.
- Keys: with a comp section selected, Alt+Up/Alt+Down switch it to the take above or below.
- Menus: a take's (use whole take, listen, rename, delete, move to a new track), the track's
  (show takes, flatten comp, delete unused takes).

## Stages

1. Model, file, history and ids, with `Takes.cpp` and its tests.
2. Recording: loop laps, takes, folding; audition and crossfades in the bridge.
3. The arrangement: lanes, comping input, badges, toggle, menus, keys.
4. Docs, render scenes, a demo.

## As built

- Auditioning lives in `AudioBridge` (`setAudition`), not on the track: the view asks for it, the
  bridge plays the lane's clips, and nothing about it is saved or undone.
- While loop recording, a finished pass moves into a lane of its own at the wrap (so it stops
  playing and shows), and every pass is folded once the recording ends; an audio take gets its
  file then, the passes being one WAV at their own offsets. `foldTakes` renames the passes'
  clips after their lanes ("Take 3").
- A swipe is applied live, each frame from the comp as it was at the press, so swiping back gives
  back what it took; a click acts on release, so a press that moves becomes a swipe instead.
- The lane header's buttons are listen, use (an up arrow: into the comp) and delete; hovering one
  says what it does in the header's caption (the toolkit has no tooltips).
- The bridge's crossfade at an internal boundary is 10 ms (`AudioBridge::kCompFade`).
- Deviceless engines can be given input channels (`AudioSystem::setOfflineInputs`) so the
  recording session can be tested end to end.

## Tests

- project suite: `compRange` on audio and MIDI (cutting, merging, offsets through a tempo
  change), boundaries, flatten, the file round trip, ids, undo labels.
- audio suite: loop recording three laps into takes at the right offsets; a comp plays the
  chosen parts; the crossfade at a boundary; auditioning a lane.
- render suite: a scene with three takes and a comp; swipe, click, double-click and boundary
  checks.
