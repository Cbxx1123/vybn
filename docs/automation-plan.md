# Automation

Parameters that change over the song: a track's volume and pan, and any parameter of any device
(built-in effects, the synth, the sampler where it can, CLAP plugins), on tracks and on the master.

## Model

- `audio::AutomationLane` (`src/audio/Automation.h`): a parameter id and points `{beat, value}`,
  sorted, beats from the timeline's start, values as knob positions 0-1 along the parameter's own
  curve (a fader's taper for volume, a log sweep for frequencies), joined by straight lines and held
  for switches and choices. A lane with no points is kept (it is shown) but plays nothing.
- Where lanes live: `Track::automation` for the track's own volume (`kVolumeParam`) and pan
  (`kPanParam`), and `DeviceSetting::automation` for a device's parameters (by its parameter id). A
  device's lanes travel with it: reordered, copied, removed and undone with it, no ids needed.
- Lanes are content: compared by the history (undo steps "Edit automation"), saved in the project
  file (`automation = [{ param = ..., points = [...] }]` under the track and under each device), and
  edited through `ProjectStore::editTrack`, `editDevice` and `editMasterEffects`.

## Audio

- `audio::AutomationCurve`: a lane made ready for the audio thread on the main thread, with the
  parameter's range and curve (`ParamCurve`: min, max, log, stepped) so the audio thread turns a
  position into the plain value the device takes, without asking the device.
- `DeviceChain` holds the curves in its immutable `Layout` (per device, plus the track's own mix
  lanes), published and retired as the devices already are (`setAutomation`). Each block, before a
  device runs, the chain evaluates its curves at the block's start beat and calls the new audio-thread
  `Device::automate(param, value)`:
  - built-in effects set their parameter atomics, the synth its stored parameters (both taken at the
    start of the block), plugins get a `ParamValue` event at frame 0 of the block they process.
  - instruments are automated through a new `Instrument::automate(transport)`, which `NotePlayer`
    calls before `beginBlock`, so the synth's block starts with the automated values.
- `TrackProcessor` takes volume and pan from its chain's mix curves (when they have points) in place of
  the project's values; the level already glides a sample at a time, so steps don't click.
- Values follow the transport's position whether or not it plays (a stopped song sounds as it would
  at the playhead), and exports render them, since they go through the same `AudioBridge`.
- `AudioBridge` rebuilds a chain's curves when the track's content, the master's or a device's
  parameter list changed (a plugin loading), and ignores a plugin's own reports of automated parameters.

## Arrangement

- Rows have heights: a track with automation shown is its row plus one lane per automation lane
  plus a strip with "Add automation". `rowAt`, `clipRect`, scrolling, the reorder drag and the drop
  target use the row table instead of a fixed height.
- Each track header gets an **A** toggle (shown or not; a view setting, not saved, kept by track id),
  in the header's radial menu of toggles with M, S, R and W (`RadialToggles`).
- A lane's header (in the header column): its name ("Volume", "Reverb: Mix"), the value at the
  playhead, and a remove button. The lane: its curve and the area under it, its points.
- Editing (snapped to the grid unless Shift): click to add a point, drag one to move it (between its
  neighbours), double-click or right-click one to remove it, Alt-drag to draw a line of points.
- "Add automation" opens a menu of the track's volume and pan, then each device's parameters.

## Devices panel

- A knob with an automation lane shows the automated value at the playhead, and its value reads
  "Auto ...", so what it shows is what plays (turning it changes the project's own value, which the
  automation leaves alone while it has points).

## Recording

- Each track header gets a **W** toggle beside A (in its radial menu): the track's writing armed (kept by track id in
  `AutomationWriter`, not saved). While the transport plays and the track is armed, moving its
  mixer fader or pan knob, or any knob, switch or choice of its devices (the Devices tab's cards
  and a device's own view), writes into the matching lane at the playhead instead of changing the
  project's own value. A lane that doesn't exist yet is made.
- Latch: from the first touch the lane keeps taking the last value at the playhead every frame
  (`AutomationWriter::update`, in DawApp's first frame hook) until playback stops. The points in the
  range written are replaced. The pure logic is `audio::AutomationWrite` (`beginAutomationWrite`,
  `writeAutomation`, `finishAutomationWrite`):
  - at the touch, a point a 1/128 beat earlier holds the lane's own value up to it (when the lane had
    points), then the written value;
  - each write removes the points between the last write and the playhead and adds one there; a
    value held still moves the last point on instead, so a long pass stays small while it runs;
  - the playhead going back (a loop, a seek) finishes the stretch so far and begins another there;
  - finishing (playback stopped, the track disarmed) puts the lane's own value back a 1/128 beat
    after the pass (when the lane had points) and thins the pass alone with `thinAutomationPoints`
    (tolerance 0.004), so the points around it are left as they were.
- Undo: the edit isn't settled while anything is being written (`writing()`), so the history takes
  the whole pass as one step, "Edit automation", once playback stops.
- The mixer's fader, pan knob and readout show what plays at the playhead (`trackValueAt`), as the
  device knobs already did, so a fader being written follows the hand and an automated one moves.

## Status

Built as above, with these limits, each a possible next step:

- Values are set once a block, at its start (blocks are at most a few milliseconds; the track's level
  glides a sample at a time). Splitting blocks where curves move would make built-in effects
  sample-accurate; plugins take one event per block.
- Recording latches by default; Touch mode (the Transport menu) stops writing when the control is let
  go and glides back to the lane over a quarter beat (see docs/routing-next-plan.md, section 6). It writes once a frame (about 60 points a second, before thinning), so a fast knob
  move is as smooth as the frame rate. The arming is a view setting, not saved. The master has an
  arming of its own (a flag, its header's W toggle): with it, the master fader and the master's
  devices' knobs write into `Project::masterAutomation` and the master effects' lanes (added after
  this plan; see `docs/routing-next-plan.md`).
- The master volume has a lane too (`Project::masterAutomation`, the volume only; the arrangement's
  master row shows it with the master effects' lanes).
- A plugin's parameters are listed for automation once it has loaded; its lanes play from then.

## Tests

- Project: lanes round-trip through the file, undo steps, `sameContent`, the store's revisions.
- Recording ("Automation recording:" in the project self-test): the touch's hold point, a held value
  adding no points, replaced points, the lane's own value back after the pass, a ramp thinned to its
  ends, a loop's second stretch, a stepped lane; and `AutomationWriter` with a fake playhead: nothing
  written while stopped, unarmed or for a master whose writing is off, the fader and a Gain knob written from touch to
  stop with the own values untouched, one undo step for the pass.
- Audio: a volume ramp renders as a ramp; a built-in effect's parameter (Gain) follows its lane; the
  synth's follows; a plugin gets `ParamValue` events; values hold at the ends and step for switches.
- Render: the arrangement with a track's automation shown (golden image).
