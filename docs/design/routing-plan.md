# Routing: sends, return tracks and group buses

Until now every track went straight to the master. This adds two kinds of track that take other
tracks' sound instead of making their own, and the paths into them:

- **Return tracks** (`TrackKind::Return`) are fed by **sends**: any track can send a share of its
  sound to any return, before its fader or after it. A return has effects, a fader, pan, mute, solo
  and automation like any track, and goes to the master (or a group).
- **Group tracks** (`TrackKind::Group`) are fed by tracks whose **output** is the group, in place of
  the master. Groups can go into groups.
- **The master volume** is part of the project (saved, undone), after the master effects. The old
  master fader, the audio system's monitoring gain, stays as the master strip's "Out" knob (not part
  of the mix: exports never hear it, and `--silent` still sets it to zero).

## Model

- `Send { ObjectId to; float level; bool preFader; }`: the level is linear gain 0-1 (0 dB at most),
  shown on a fader's taper. `Track::sends` holds only the sends that exist (a knob turned up from
  nothing makes one). `Track::output` is a group's id, or `kNoId` for the master.
- Sends and the output are mix fields (`editMix`, `mixRevision`), like gain and pan: undo steps
  "Change send" and "Route track". Send automation lanes are the track's own lanes, with the
  parameter id `sendParam(to)` (`0x40000000 + id`), so they are content like the volume's.
- `Project::masterVolume` (linear 0-2) is the master's (`editMasterVolume`, `masterRevision`), undo
  step "Master volume".
- `daw/model/Routing.h`: `reaches` (whether one track's sound gets to another), the outputs and returns a
  track may choose (none that would loop), `forgetTrack` (a deleted track's sends and the outputs
  into it go: `Project::deleteTrack` calls it), and `planRouting`, the pure function the bridge uses.
- File: `kind = "return" | "group"`, `output = <id>`, `sends = [{ to = <id>, level, pre_fader }]`,
  automation `param = "send", to = <id>`, `master_volume`. Read back refusing a send to anything but a
  return, an output to anything but a group, an id no track has, and loops, each with a reason.

## Engine

- `audio::Routing` (`src/audio/Routing.h`): the tracks in **stages** (a track's stage is one more
  than the latest of the tracks feeding it; plain tracks are stage 0) and, for each, its output (a
  bus, or the master) and its sends (the bus, the send's slot, its delay, pre or post). Made on the
  main thread and handed over through the AudioSystem's command queue (`setRouting`), in order with
  the processors' adds and removes, so it never names a processor that is gone; one the audio thread
  has replaced is deleted on the main thread. With no routing the engine mixes as before (tests).
- `AudioSystem::render`: stage by stage, each across the real-time pool, then each track of the stage
  routed in order on the audio thread (`TrackProcessor::routeInto`): its sound added into its
  output's input (or the master) and each send into its bus's input. A bus adds what it was sent to
  its own sound when it renders.
- `TrackProcessor`: the delay line it already had for delay compensation is read at more than one
  delay: its output at its compensation, each send at its own. The gain, pan and mute are worked out
  as a ramp once a block and used for the output and the post-fader sends; pre-fader sends follow
  only the mute (and solo). Send levels are per slot (64: `audio::kMaxSends`) and glide, and follow automation.
- `AudioSystem::masterVolume()`: the project's master volume, after the master effects and before
  the monitoring gain.

## Delay compensation

`planRouting` takes each track's own plugin latency and lines the whole graph up: in stage order,
each track's input time is the latest arrival among what feeds it, its output leaves its latency
later, the master's time is the latest arrival into it, and each path (an output, a send) is held
back by the difference between when its destination needs it and when it arrives. Each track's
output delay is its `compensation`; each send's is in the routing.

## Solo

Soloing a track keeps what it feeds audible (its group, the returns it sends to), and soloing a
group or a return makes what feeds it audible. The bridge works out each processor's solo from the
graph; the project keeps only what the user pressed.

## UI

- Add track menus (Project menu, the mixer's +): Instrument, Audio, Return and Group track.
- Mixer strips: the output (a button naming it: "Master" or a group; a menu of the choices that
  wouldn't loop) and a send knob for each return (lettered A, B, ...; right-click for pre or post
  fader and removing it). Bus strips have no record arm. The master strip's fader is the project's
  master volume, with the monitoring gain beside it as "Out".
- Arrangement: bus rows take no clips (no double-click clip, no clip dropped onto them; a clip only
  moves onto a track of its own kind), have no record arm, and the caption names the kind and the
  output. "Add automation" lists each send.
- Recording automation: the send knobs write into their lanes like the fader.

## Status

Built as above, with these changes from the plan:

- The engine first routed each stage serially on the audio thread (each track adding its output and
  sends into its buses' inputs after the stage rendered). The routing bench measured that at +150%
  over plain tracks for 96 sends. Buses now **pull** instead: as a bus renders (in parallel with the
  other buses of its stage) it reads what feeds it straight from its sources, which rendered in an
  earlier stage and hold still: their outputs, and their delay lines at each send's delay. A send's
  level glides on the one bus it feeds, and a settled level or mute is one number for the block, not
  a ramp. Only the tracks that go to the master are added serially, as before.
- The mixer's sends are slim bars (the knob widget is 48 px at least, which pushed the strip's
  toggles out of a mixer of ordinary height).
- Fixed on the way: a clip dragged onto another track only lands on one of its own kind (an
  instrument clip could be dropped onto an audio track).

Measured (`VYBN_TEST_BENCH=1 output/vybn --project-selftest`, the Debug build at -O0): 4 s of 24
synth tracks take about 160 ms plain and 245-255 ms with every track sending to 4 returns (96 sends,
half pre-fader) and going into 2 groups (+50%, from +150% with the serial routing). A track with no
sends costs what it did. The bench's sends are all live; a send turned down costs nothing once settled.

Limits, each a possible next step:

- ~~No sidechain (a compressor keyed by another track)~~: Done, a key is a read of the source's delay line, not a feed: see "Sidechain" and "Plugin sidechain" in docs/design/routing-next-plan.md.
- ~~A send goes to a return, an output to a group~~: a send may now go to a return or a group (an output
  still only to a group): see "Flexible routing" in docs/design/routing-next-plan.md.
- ~~One output per track; 16 sends per track are played~~: one output still, but any number of sends into
  groups and returns, up to 64 per track (refused past that, not dropped).
- ~~Groups don't fold their tracks in the arrangement; the output shows in the mixer and the header.~~ Done: see "Group folding" in docs/design/routing-next-plan.md.
- ~~The master volume can't be automated~~: it can now (`Project::masterAutomation`, the arrangement's
  master row, the mixer's master fader; see `docs/design/routing-next-plan.md`), and the master's devices are
  written when the master's writing is armed.

## Tests

- Project: the file round trip (kinds, output, sends, send lanes, master volume), refusals (unknown
  ids, a send to a group, an output to a return, a loop), clamping, undo steps and labels, a deleted
  return's sends going and coming back with undo, `reaches` and the choices.
- `planRouting`: stages, loops dropped, delays for latencies on tracks, buses and sends.
- Bridge (deviceless engine): a post-fader send at half level, a pre-fader send with the fader down,
  a muted track's sends silent, a group's gain and effect applied once to the sum, nested groups, solo
  keeping returns and groups, send automation, master volume, and a routing change making no
  processors.
- Engine bench: 24 tracks with 4 returns and 2 groups against 24 plain tracks.
- Render: the mixer with returns and a group (golden image).
