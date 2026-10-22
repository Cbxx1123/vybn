# Routing, part 2: master automation, group folding, sidechain, flexible routing

The four limits left by `docs/routing-plan.md`, each built in three waves by one developer:

1. **Prototype**: the model, the file, undo and the engine path working end to end, with tests that
   prove the core behaviour. No polish, the least UI that shows it works.
2. **Flesh out**: the full UI and the edge cases (rules, loops, deleting, undo names, recording),
   with tests for each.
3. **Polish and optimise**: a render golden looked at and explained, draw caches keyed right
   (`VYBN_VERIFY_CACHE=1` quiet), the audio thread's cost measured (a bench where it touches the
   audio path) and cut, docs (`CLAUDE.md` and this file's Status), every suite passing.

The features run one after another, in this order, since they share files (no git: one checkout).

## Rules for every wave (read before starting)

- Read `CLAUDE.md` first, then `docs/routing-plan.md` (how routing works), then the files named below.
- Build: `cmake --build build -j 8` (from `UI-Engine/`). Tests: `bash tools/test.sh` (every suite,
  about 15 s); one suite: `./output/vybn --project-selftest` etc. Syntax-check one file without
  building: `bash tools/check.sh FILE`. Never use `C:/MinGW` (32-bit).
- Line endings: files are a mix of CRLF and LF. Edit with the Edit tool. Never `sed -i` a CRLF file
  (it strips the CRs). Check with `file PATH` if unsure.
- Match the code around you: its comment density, naming and voice (plain sentences, no marketing).
- `Track` changes trip the `static_assert(sizeof(Track) == ...)` in `src/daw/History.cpp`: add the new
  field to `shellOf`, `sameTrackFields`, `sameSavedTrack` and `ProjectStore::restore` (or say why it is
  not part of the history), then update the size. Project-level fields go in `rootOf`, `sameContent`,
  `sameSaved` and the restore plan. Device fields go in `DeviceSetting::operator==` and
  `sameSavedDevice`.
- New `.cpp` files go in `CMakeLists.txt`'s `add_executable` list. New render scenes go in
  `kScenes` in `tests/RenderSelfTest.cpp`; write a golden with
  `./output/vybn --render-selftest --scene NAME --update-goldens` only after looking at the picture
  (Read the PNG) and checking it shows what it should.
- The audio thread never locks, allocates or frees. Buffers are sized in `prepare`.
- Leave every suite passing at the end of the wave. If a golden changes, say which and why.
- Don't run the app for more than a few seconds and never leave it running (`output/vybn.exe` must be
  free for the next build).
- Report at the end: what you built, the files you touched, the tests you added (with their output
  lines), measurements, anything left undone or unsure, and anything you noticed that the next wave
  should know.

## 1. Master automation

The master's volume (and its effects' parameters) automated like a track's, and the arrangement
showing the master.

- **Model**: `Project::masterAutomation` (`std::vector<audio::AutomationLane>`; only `kVolumeParam`
  for now). Master effects already have `DeviceSetting::automation`. Edited through a new
  `ProjectStore::editMasterAutomation()` (moves `revision` and `masterRevision`). History: a master
  field (`rootOf`, the plan's `master` flag, restore, `sameContent`, `sameSaved`), named "Edit
  automation". File: `master_automation = [...]` at the root, written like a track's lanes (param
  `"volume"`), read with the same clamping and refusals.
- **Engine**: `AudioBridge::sync` passes `&project.masterAutomation` as the master chain's mix lanes
  (`syncAutomation(master, nullptr, project.masterEffects, &project.masterAutomation)`), and
  `AudioSystem::render` takes the master volume through `masterEffects_.automateMix` (or
  `automateMixParam(kVolumeParam)`) at each block's start, in place of `masterVolume_`'s value when the
  lane has points.
- **Arrangement**: a "Master" row below the tracks: a header (name "Master", an A toggle only, no
  clips in its lane area) and, while shown, its lanes: the master volume, then each master effect's.
  `AutomationPlace` gains the master (e.g. `bool master` with `own` meaning the master's own lanes);
  `lanesAt`/`editLanesAt`/`automationRows` take it. "Add automation" on the master row lists Volume
  and each master effect's parameters. Row indexing: keep tracks at `0..n-1` and the master row at
  `n` (or a separate member), and make sure `rowAt`, reorder drag, clip drag and selection never treat
  it as a track.
- **Mixer**: the master fader shows the automated volume at the playhead (as `trackValueAt` does for
  tracks); the `AutomationWriter` can write the master volume (`touchMaster`) and the master effects
  (lift "The master's devices are never written").
- **Waves**: prototype = model, file, history, bridge and engine, with a test that a master volume
  lane renders as a ramp (deviceless engine, as `tests/BridgeSelfTest.cpp`'s automation checks do);
  flesh out = the arrangement's master row and lanes, the menu, the mixer showing and writing it,
  undo/recording tests; polish = a render scene `master` (the arrangement with the master's lanes
  shown) golden, cache keys, docs.

### Status

Built as above, with these changes from the plan:

- `AutomationPlace` gained `bool master`: `AutomationPlace::masterVolume()` (`own` and `master`) and
  `masterEffect(i)` (a device place on `DevicePlace::effect(true, 0, i)`); `lanesAt`/`editLanesAt` ignore
  `track` for them, and `masterAutomationRows` lists the volume then each effect's lanes.
- The master's row in the arrangement is its own `Row master_` after the tracks, not a track index; only
  the lane functions give it the index `tracks.size()` (the old "below the last track" slot, so a drop
  highlight still lands there). Its header (`MasterHeader`: name, A, and W when the app records) is 56 px
  and holds no clips; its lanes and add strip draw and edit as a track's. The shown state is a bool
  (`showMasterAutomation`), not in the track-id set.
- Writing has an arming of its own for the master (`AutomationWriter::masterArmed`, a flag, not saved; the
  header's W) instead of the track's: `touchMaster` writes the volume lane and `touch` takes master device
  places, so the master effects' knobs record too. A master pass is flagged and has no track. The mixer's
  master fader and readout show `masterVolumeAt` at the playhead and write the lane while armed.
- The render scene `master` (the demo with the master's volume lane and a Gain effect's lane shown, 1200x740)
  is a golden and a cache scene: a settled frame draws from its caches. The arrangement's cache key gained
  the master row's open state; lane edits are covered by the store's revision, and the playhead-dependent
  value text and the headers are drawn outside the cache, like a track's. `VYBN_VERIFY_CACHE=1` is quiet on
  the scene and on `--demo masterautomation` (the master row's lanes shown, writing armed, playing).
- A demo, `--demo masterautomation`, was added for checking it in the app.

Tests: the file, history and engine checks (`masterAutomationChecks`, wave 1), `lanesAt`/`masterAutomationRows`,
the writer's master pass (one undo step) and the strip's value following the playhead (project suite), and
`masterEditChecks` on a laid-out `ArrangementView` (click, drag, right-click, the add strip, the lane's x, the
row not being a track).

Limits:

- Only the volume is a master lane of its own (the file refuses `"pan"` and `"send"`); the master has no
  other mix parameters.
- The master's row has no way to remove all lanes at once and no reordering; "Add automation" lists the
  master effects' parameters once they have loaded (a plugin's), as a track's menu does.
- The mixer strip's fader writes while the song plays and the master's W is on, latching or touching as
  tracks do (see section 6).

## 2. Group folding

A group track can be folded in the arrangement: the tracks that go into it (directly, or through
groups inside it) are hidden, and the group's lane shows a summary of their clips.

- **State**: view state like the A toggle (`ArrangementView`, a `std::set<ObjectId>` of folded
  groups, not saved, not undone). `ArrangementView::fold(track, bool)` / `folded(track)`.
- **Arrangement**: a group's header gets a fold control (a small triangle or chevron button at its
  left, or an "F" in its radial toggles: pick what reads best and fits the header) and, when folded,
  "N tracks" in its caption. Hidden rows take no height (`layoutRows`), aren't hit by `rowAt`, aren't
  drop targets, and their lanes aren't drawn. Tracks inside groups have their header indented by their
  depth (how many groups deep) with a thin accent bar in the group's colour or the accent, so the
  nesting reads at a glance. A folded group's lane draws its children's clips as faint blocks.
  Selecting a track that is hidden (the mixer, the keyboard) unfolds its groups.
- **Mixer**: folding hides the children's strips too (rebuild on fold change), with the group strip
  showing the same fold control.
- **Waves**: prototype = fold state, hidden rows, the control; flesh out = indentation, depth bars,
  summary blocks, the mixer, selection unfolding, "Fold all groups"/"Unfold all" (View menu);
  polish = render scene `folding` golden, the arrangement's draw cache key including the fold state
  (`VYBN_VERIFY_CACHE`), tests for `rowAt` and layout with folded groups, docs.

### Status

Built as above, with these changes from the plan:

- **State**: `Track::folded` (groups only; the plan had view state, but the user asked for it to be saved). Like `armed` it is a mix field changed through `editMix`, so it marks the project modified but is no undo step: `shellOf`, `identicalShell` and `sameSavedTrack` carry it, `sameTrackFields` and `sameMixFields` leave it out, a structural restore keeps the current value by track id, a mix restore doesn't touch it, and undoing a group's delete brings it back as last recorded. The file has `folded = true` in a folded group's table (read back for groups only). `FoldState` (`FoldState.h`) is the arrangement's and the mixer's view of it over the store (`folded`, `set`, `clear`, `groups`, and a `version()` hashed from the folded ids and recomputed only when the store's revision moves), owned by `DawApp` and handed to `ArrangementView` and `MixerView` (each makes its own over the same store when given none, as the render scenes do), so folding in either shows in the other and an opened project, an undo or a demo shows the saved state. `ArrangementView::fold`/`folded`/`hidden`/`foldAll`/`unfoldAll` wrap it. View > Fold all groups / Unfold all groups (`view.foldAll`, `view.unfoldAll`, no default keys).
- **Rules** (`Routing.h`): `groupDepth`, `outputGroups`, `insideGroup` and `hiddenTracks`, outputs only (sends never hide), loops ending the walk.
- **Control**: a `FoldChevron` where a group's number is (the F radial toggle of the prototype is gone); the mixer's group strip has one beside its name. A folded group's caption says "N tracks folded".
- **Layout**: a hidden row has height 0, no lanes, no header, clips or input; `rowAt`, drops and drags never find it; the reorder preview treats it as passed with the row before it. Headers are indented 12 px per level of depth with a 2 px accent bar per level (down the whole row, lanes included); lane headers and the add strip are indented too. A folded group's lane draws the merged spans of its hidden tracks' clips as accent blocks with a thin edge, inside the arrangement's draw cache. What folding works out (hidden flags, depths, folded counts) is redone in `layoutRows` only when the store's revision or the fold version moves.
- **Reordering** (`Routing.h`: `dragBlock`, `settleDrop`, `dropOrder`, `moveBlock`, pure and tested): dragging a header moves a block, the track alone or, for a folded group, the group and every track hidden inside it, kept together in order. A drop slot that would fall between a folded group and its hidden tracks, or among them, goes to the nearer end (before the group, or after the run's last track; a tie goes after). The selection stays on its track.
- **Selection**: a selected track that is hidden (mixer, keyboard, undo) unfolds the groups hiding it, once per `selectionRevision` change (checked in `ArrangementView::handleInput`, so it needs the arrangement to exist, as it always does in the app).
- **Mixer**: hidden tracks get no strip (rebuilt when the fold version moves).
- **Caches**: the arrangement's key gained the hidden flags and the fold version; `FoldChevron` has a cache keyed by its state. `VYBN_VERIFY_CACHE=1 VYBN_VERIFY_STORE=1 ./output/vybn --silent --demo folding` for 8 s printed only FRAME lines.
- **Demo and scenes**: `--demo folding` (two nested groups, the inner folded, playing). The render scene `folding` (a golden and a cache scene) shows the nested-groups project with Outer open, Inner folded and a lane open on a nested track; the `mixer` scene is now 1500 wide so its group strip's chevron shows (golden updated).

Tests: `foldChecks` and `foldProjectChecks` in the routing suite (depth, `insideGroup`, `hiddenTracks`, loops; the file round trip and the ignored flag, not an undo step but modified, kept through undo, an undone delete, the drag rules and `moveBlock`) and `foldChecks`, `foldUiChecks`, `foldMixerChecks` and `foldDragChecks` in the render suite (layout, lookups, chevron click, indentation, counts, selection unfolding, Fold all, `FoldState` versions, mixer strip counts, a project opened folded, a folded group dragged with its tracks, a drop among hidden tracks), plus the `folding` scene.

Limits:

- ~~The selection rule lives in the arrangement~~: it lives in `FoldState::revealSelection` now, which both
  views call, so the mixer alone unfolds too (section 6).

## 3. Sidechain

The built-in Compressor and Gate can be keyed by another track's sound (ducking a pad under a kick).

- **Model**: `DeviceSetting::sidechain` (an `ObjectId`, `kNoId` for none; the device's own input
  otherwise). Only effects whose `EffectInfo` says `sidechain = true` (Compressor, Gate) take one.
  Saved as `sidechain = <id>` in the device's table, refused when no track has the id or it would loop.
  Compared in `operator==`/`sameSavedDevice`; undo names it "Sidechain". `forgetTrack` clears
  sidechains to a deleted track. `daw::Routing`: `canSidechain(project, track, source)` (not itself,
  and the source mustn't be fed by the track, so no loop), `routingError` checks them.
- **Plan**: a sidechain is an ordering edge (the source renders in an earlier stage) and a timing
  edge (the keyed track's input time is at least the source's output time), but carries no sound to
  the master. `RoutingPlan::Node` gains `keys` (per keyed effect: the source track and its delay).
- **Engine**: `audio::Route` gains key feeds (`KeyFeed { TrackProcessor *from; uint32_t effect;
  uint32_t delay; }`). Before its chain runs, a track fills a key buffer per keyed effect (at most
  `kMaxKeys`, 4) from the source's delay line at the key's delay: the source's sound after its effects
  and before its fader (not silenced by its mute: a muted kick still ducks). The chain hands each
  keyed effect its key (`Effect::setKey(left, right)` before `run`, cleared after); Compressor and
  Gate detect on the key when they have one.
- **UI**: a keyed effect's card (Devices panel and device view) gets a "Sidechain" dropdown: "None"
  and the tracks it may take (`canSidechain`).
- **Waves**: prototype = model, file, plan ordering, engine path, Compressor keyed, test (a kick
  track keys a compressor on a held pad: the pad's level dips on each kick and not between);
  flesh out = Gate, the dropdown, rules and refusals, deleting the source, delay compensation for
  keys, undo name; polish = render scene of a keyed compressor card golden, a bench (keys cost),
  docs.

### Status

Built as above, with these changes from the plan:

- **Model**: `DeviceSetting::sidechain` is a `uint32_t` (0 = none; the audio layer doesn't know `ObjectId`). Compared in `operator==` and `sameSavedDevice`; the undo step is named "Sidechain" (`describeEffects`). The file reader runs `routingError` after the master's effects are read, so it checks them too.
- **Master effects can be keyed** by any track (the master runs after every stage, so no loop is possible): `Routing::masterKeys`, read by `AudioSystem::render` into four key buffers of its own before the master chain runs; `RoutingPlan::masterKeys`.
- **A key is a route**: `graphOf` has an edge from the source to the keyed track, so `reaches`, `canOutputTo`, `canSendTo`, `canSidechain` and `routingError` see loops through keys (two tracks keyed by each other, or a key from a track the keyed track feeds, are refused in a file and offered nowhere); `planRouting` builds key edges in track order like sends and drops one that would loop. Solo ignores keys (a muted kick still keys; a soloed pad doesn't solo it).
- **Delay**: a key's delay is the keyed track's input time minus the source's output time (the master's: the master's time minus the source's output), clamped to `kMaxCompensation`; it is read from the source's delay line at that delay, after its effects and before its fader, not silenced by its mute.
- **Gate** is keyed too (it was a wave 2 item): both Compressor and Gate detect on the key (`Effect::setKey`, `keyLeft_`/`keyRight_`).
- **Limits are refused, not ignored**: at most `audio::kMaxKeys` (4) keyed effects on a track and on the master. `routingError` refuses more in a file, and the Sidechain control offers none for a further effect and says "4 sidechains per track at most" (`sidechainOffer`, `kMaxTrackKeys`, `kMaxMasterKeys`).
- **UI**: a Compressor or Gate card (Devices panel) and a device's own view have a Sidechain row (`addSidechainControl`: a `Dropdown` of None and the tracks it may take); a keyed card shows "Keyed by <track>" in the accent under its kind. `DeviceAccess` gained `sidechain` and `revision`: the offer is worked out only when `ProjectStore::revision()` moves. `ui::Dropdown::choose(index)` was added for tests. Deleting the source clears the key, in the same undo step as the delete.
- **Demo and scene**: `--demo sidechain` (a pad with a compressor keyed by a muted kick track, the Devices tab showing it, playing) and the render scene `sidechain` (the Devices panel on a pad: a keyed Compressor, an EQ and a Gate; a golden and a cache scene).

Tests: `sidechainModelChecks`, `sidechainPlanChecks`, `sidechainEngineChecks`, `sidechainGateChecks` and `sidechainDelayChecks` in the routing suite (the file and its refusals, the rules, deleting and undo, the plan, the pad dipping under a muted kick: -41.1 dB during the kicks and -24.8 dB between against -24.8 dB throughout without a key, the same on the master, a gate opening on the kicks and closing to its range, an impulse key's gain change landing 0, 100 and 250 frames late for those delays), and `sidechainUiChecks`, `sidechainDeviceViewChecks` and the `sidechain` scene in the render suite.

Measurements (Debug build, this machine):

- The routing bench (`VYBN_TEST_BENCH`): 4 s of 24 synth tracks and a kick: no effects about 184 to 233 ms, a compressor each 261 to 302 ms, each keyed by the kick 273 to 313 ms: keys cost about 4 to 5% over unkeyed compressors (the key copy is two contiguous runs per keyed effect, so it was left as it is).
- The offers of 8 cards on a pad in a 64-track project with 4 groups: 21.9 ms when worked out every frame (the first version, a routes graph made again per track), 0.65 ms with the graph made once, and not at all on a still frame: a settled Devices frame went from 22.6 ms to 0.9 ms.
- `--demo sidechain` for 8 s with `VYBN_VERIFY_CACHE=1 VYBN_VERIFY_STORE=1 VYBN_FRAME_STATS=1 --silent` printed only FRAME lines.

Limits:

- A keyed effect listens to one track; a Compressor or Gate can't mix several keys, and there is no listen mode or external-key level. (A detector high-pass, "Detect HPF", came in section 6.)
- The built-in Compressor and Gate take a key, and so do CLAP effect plugins with a sidechain port (section 5); the Limiter and the other built-ins don't.
- A track plays at most 4 keyed effects and the master 4; the project refuses more.
- The key is the source's sound before its fader, so moving the source's fader doesn't change the ducking.

## 4. Flexible routing

More than one destination per track: sends may go into groups as well as returns, so a track can
feed several buses, and the 16-send cap goes.

- **Rules**: `canSendTo` allows a group too (still no loops, not itself, not the group it already
  outputs to); `routingError`, `planRouting`'s `allowed` and the file reader follow.
- **Engine**: nothing changes in how sends play (a bus pulls what feeds it); `kMaxSends` rises to 64
  (check the memory: the per-track arrays) or sends get their levels from the route.
- **UI**: the mixer keeps send bars for returns; the output menu gets "Also to" toggles for the
  groups (a send into the group at 0 dB, post fader), and the output label reads "To Group 1 + 1".
  The arrangement caption likewise.
- **Waves**: prototype = rules, file, plan, cap, test (a track feeding two groups is heard through
  both); flesh out = the menus and labels, deleting and undo, loops refused; polish = mixer golden
  updated, docs, the routing bench rerun.

### Status

Built as above, with these changes from the plan:

- **Rules** (`daw/Routing`): `canSendTo` allows a return or a group (`isBus`), still not itself or anything that loops. A send to the group the track already outputs to is **refused** (it would play twice): `canSendTo` says no, `routingError` says "which is also its output", and `planRouting` drops such a send and keeps the output. The reverse holds too: `canOutputTo` doesn't offer a group the track sends to, so a project can't be made the file reader would refuse. A track reaching one bus by two paths (an output into G1 and a send into G2 that goes into G1) is allowed. `routingError`'s message is now "isn't a bus (a return or group track)".
- **Cap**: `audio::kMaxSends` is 64 (`TrackProcessor::kMaxSends` takes it); the per-slot arrays stayed (12 bytes a slot, `sizeof(TrackProcessor)` 1080 to 1656 bytes). `routingError` refuses more ("at most 64 are allowed") and `canSendTo` offers no further send, so none is dropped silently.
- **Engine**: nothing new for sends into groups (a bus pulls its feeds); checked: a group fed by outputs and sends, delay compensation (a send into a group is an edge of `planRouting`, so its time and delay count), solo spreading through it.
- **UI**: the output menu (mixer strip and the track's Output submenu) has an "Also to" section of toggles (`alsoToChoices`; `setAlsoTo`: a post-fader send at 0 dB, or the send and its lane out, one edit, undo steps "Add send"/"Remove send"). The strip's label is `outputLabel` ("To Group 1 + 2", returns not counted, the name cut with ".." at 17 characters) and a bus's arrangement caption adds " + N". Each existing group send has a `SendBar` labelled "G1" (or the name's first two characters) under the return bars; "Add automation" lists groups too. With more than 3 bars they sit in a ScrollView of 3 rows, with narrower bars, so a track with many sends keeps its M/S/R row inside the strip. `MixerView` makes its strips again when `groupSendShape` (the sends into groups) changes, checked when the store's revision moves.
- **Deleting** a group takes the sends into it and their lanes (`forgetTrack`); one undo brings them back.
- **Demo and scene**: `--demo flexible` (three tracks into Group 1, one also sending to Group 2 and a return, the mixer showing, playing). The `mixer` scene gained a second group and the first track's output to Group 1 with a send to Group 2: "To Group 1 + 1" and a G2 bar (golden updated).

Tests: `flexibleModelChecks`, `flexiblePlanChecks`, `flexibleMenuChecks` and `flexibleEngineChecks` in the routing suite (the rules and refusals, the file, the cap, the plan, the menus' choices and edits, labels, deleting and undo, and the engine through the bridge: output to Group 1 plus half to Group 2 at 1.0609 of the track alone, wanted 1.0607, each path alone, 20 and 64 sends all heard); `flexibleMixerChecks`, `tallStripChecks` (12 send bars: the toggles stay inside the strip) and `faderDragChecks` (64 tracks, the strips aren't made again) in the render suite.

Measurements (Debug build, this machine, which ran the plain case about 205-225 ms against 160 ms when the plan was written):

- The routing bench (`VYBN_TEST_BENCH`): plain 218-224 ms; 4 returns (96 sends) and 2 groups 309-311 ms (+38 to +42%); the same with each track also sending into the other group (120 sends) 316-321 ms, 2-3% more. Sends into groups cost what sends into returns do.
- A 64-track project's frame: 0.94 ms still and 0.97 ms with a fader dragged every frame (the revision moving); `groupSendShape` costs about 32 us (112 sends), once a frame while the revision moves.
- `--demo flexible` for 9 s with `VYBN_VERIFY_CACHE=1 VYBN_VERIFY_STORE=1 VYBN_FRAME_STATS=1 --silent` printed only FRAME lines.

Limits:

- A send into a group is post-fader at 0 dB when made from the menu (the bar's menu sets pre-fader); a group's own send bars exist only while the send does.
- More than 64 sends per track is refused; the send slots stay in the track, not in the route.
- ~~Two groups named alike had send bars that read alike~~: matching short names are numbered ("Dr1", "Dr2"; section 6).
- A group a track outputs to can't also be sent to (a deliberate rule); to double it, route through another group.

## 5. Plugin sidechain

Sidechain (section 3) for CLAP plugins: a plugin effect that has a sidechain input port takes a key
like the built-in Compressor and Gate.

- **Which plugins**: an effect plugin whose CLAP `audio-ports` extension lists an input port that is
  not the main one (`CLAP_AUDIO_PORT_IS_MAIN` unset), with 1 or 2 channels: its first such port is its
  sidechain. The scan learns it (`PluginInfo`/the host's info message gains the sidechain port's
  channel count, 0 for none; the scan cache's format follows), so `takesSidechain` can answer for a
  plugin device without it being loaded, and the dropdown appears on its card.
- **Sandbox**: the shared block (`src/plugin/Protocol.h`) gains `key[kMaxChannels][kMaxFrames]` and
  `keyChannels` (0: no key this block). The app copies the key in with the main input; the host hands
  it to the plugin as the sidechain port's buffer (`audio_inputs[sidechainPort]`), and silence when
  there is no key (a sidechain port must always get a buffer). The block's round trip must not slow:
  copy only the frames used, and only when keyed.
- **App side**: `PluginEffect` takes `Effect::setKey` like the built-ins do (the chain already hands
  each keyed effect its key) and passes it to its `Slot`/`Instance` for the block.
- **Test plugin**: `tests/plugins/TestPlugins.cpp` gains "Test Duck" (`vybn.test.duck`): a main stereo
  in/out and a stereo sidechain input; its output is its input times (1 - min(1, key peak in the
  block)), so a key at full scale silences it and no key passes it unchanged.
- **Tests**: the plugin suite (`tests/PluginSelfTest.cpp`, runs the real sandbox): the scan reports
  Test Duck's sidechain port and Test Gain's none; a keyed Test Duck in a `DeviceChain` ducks its input
  while a key plays and passes it when the key is silent or removed; the block round trip time is
  measured keyed and unkeyed. The project/routing suite: a plugin device with a sidechain round-trips,
  one without a sidechain port is refused. The render suite: the Sidechain dropdown on a Test Duck
  card.
- **Waves**: prototype = protocol, host, Instance/Slot/PluginEffect, Test Duck, the plugin-suite
  ducking test; flesh out = the scan info and cache, takesSidechain for plugins, the dropdown and
  file rules, a plugin that crashes or reloads while keyed (no dangling key, silence as now); polish =
  the round-trip timing, a golden of a keyed plugin card, docs.

### Status

Built as above, with these changes from the plan:

- **Protocol**: the shared block gained `keyChannels` and `key[kMaxChannels][kMaxFrames]` (`kProtocolVersion` 3). `Instance::process` copies the key in only when the block has one (and only the frames used); the host points the plugin's sidechain port at the shared key (no copy) and gives silent buffers when there is none. `PluginEffect` hands the chain's key to its slot with each 4096-frame piece.
- **The sidechain port** is the first input port that isn't the main one, with 1 or 2 channels (`Loaded::sidechainInput`, `sidechainChannels`; the same rule in the scan's probe).
- **Probe, not scan**: the scan lists descriptors as before; the manager then asks each effect plugin's sidechain port with a `Probe` request (`Instance::probeSidechain`), each in a host of its own, so a plugin that crashes when made costs only its probe: it stays listed, with sidechain 0, remembered as probed (a forced scan asks again). The scan cache is format 3 (`sidechain` and `probed` per plugin); older caches read as empty and are scanned again. `ScanStats::probed` counts the probes.
- **Rules**: `mayTakeSidechain` (a built-in with `EffectInfo::sidechain`, or any plugin) is what the file reader, the key count, the routing graph and the plan use, so the project stays pure (it can't know what is installed) and a keyed plugin that is missing or has no sidechain port keeps its key and counts toward the 4-per-track limit. `takesSidechain` (a plugin only when the scan found a port, through `setPluginSidechainLookup`, which `DawApp` points at `PluginManager::sidechainChannels`) decides the Sidechain dropdown and the "Keyed by" line; such a plugin gets neither, and the engine hands its slot the key anyway.
- **Test plugins**: "Test Duck" (`vybn.test.duck`, a stereo sidechain input; output = input times 1 minus the key's peak in the block) and "Test Crash Init" (`vybn.test.crashinit`, crashes when initialised, for the probe's robustness).
- **Demo**: `--demo pluginsidechain` (with `--plugin-folder output/plugins`): Test Duck on the pad keyed by the kick. No render scene: a keyed plugin card needs a live plugin device (a sandbox host loading in the background), which would make the render suite slower and flaky for one picture.

Tests (plugin suite): the scan lists five plugins; Test Duck's port found by the manager's scan and none for the others, the crashing plugin still listed with its failed probe remembered; the cache keeping the port and ignoring an older format; a keyed Test Duck in a `DeviceChain` and through an `Instance` (ducked, passed with no or a silent key, the key not outliving its block, a plugin without a port ignoring it); the offer functions with the manager's info; a pad keyed by a muted kick through `AudioBridge` (silenced on each kick, not between: -180 dB during, -24.8 dB between; the key removed brings back the steady level); Test Faulty crashing while keyed (silent, keyed blocks go on, keyed again after Reload).

Measurements (Debug build): a keyed block's round trip through the sandbox 10 us, the same as unkeyed (`INFO plugin sidechain:`); a plain scan of the test file (5 plugins) 117 to 176 ms, the manager's scan with its 4 effect probes (one host each) about 280 ms (`INFO plugin scan:`). `--demo pluginsidechain` for 10 s with `VYBN_VERIFY_CACHE=1 VYBN_VERIFY_STORE=1 VYBN_FRAME_STATS=1 --silent` printed only FRAME lines.

Limits:

- A plugin's sidechain is its first non-main input port of 1 or 2 channels; one with several, or a surround one, takes the first that fits.
- A plugin keyed from a project but not installed, or without a port, keeps its key (counted toward the 4 per track) and has no control.
- The probe makes and initialises each effect plugin once per scan of a changed file; a plugin that misbehaves there is simply recorded as having no sidechain.
- Only the first key a track's effect gets is used; there is no per-plugin key level or listen option.

## 6. Follow-ups: the limits and notes left, and an optimisation pass

Done by the lead alone after features 1-5, from their limits and the notes at the end of each.

- **Render suite time**: it had grown from about 7 s to 33-40 s. Each scene's window was the only one
  open, so closing it stopped GLFW and the next started it again, and starting GLFW (it enumerates the
  input devices) took about 600 ms here, its first window another 280 ms. The suite now keeps one hidden
  window open throughout: 15 s for 21 scenes and the editing checks. It prints `TIME` lines with
  `VYBN_TEST_TIMES` (the runner's `--times`), and `tools/test.sh` takes `VYBN_TEST_EXE` to test
  `output/vybn-dev.exe` while the app holds `vybn.exe`.
- **Folding**: `FoldState::revealSelection` (the selection rule) is called by both views, so the mixer
  alone unfolds a hidden track's groups.
- **Group send labels**: groups whose short names match are numbered (`groupSendLabel(project, group)`),
  and the mixer rebuilds when a rename changes them.
- **Detect HPF**: the Gate and Compressor high-pass what their detector hears (key or input), 20 Hz to
  2 kHz, "Off" at the bottom. The `sidechain` golden gained the knob (and the scene widened to fit).
- **Touch automation**: the writer's Touch mode (Transport menu, saved in the settings) ends a pass when
  the mouse is let go and glides back to the lane over a quarter beat; Latch is as before.
- **A golden of a keyed plugin card**: the `pluginsidechain` scene loads Test Duck and Test Gain in their
  sandboxes and waits until both are ready (about 2 s), so the card is the real one; it matched its
  golden exactly on repeated runs and inside the parallel runner.

### Optimisation

- **The audio code compiled optimised** (`VYBN_OPTIMISE_AUDIO`, ON): the app runs as a Debug build, so
  the audio thread ran -O0 code. At -O3 (GCC 10 vectorises only there), with debug information kept:
  24 synth tracks for 4 s 190 to about 65 ms, the export's offline render 5.3 to 16.6 times real time,
  the synth's timed render 689 to 207 ms, the engine's 24-track mix 640 to 290 ms on one thread, and
  the fast test tier's export suite 14.7 to 6.4 s (the full tier 53 to 22 s).
- **Settled levels that never settled**: `Smoothed` moves by a small share of the gap a sample, so in
  floats it stops some 1e-5 short of its target; the send smoothing and the pre-fader mute ramp tested
  for 1e-6 and so glided forever, and no send ever took its steady path. With `kSettled` (1e-4, -80 dB,
  snapped) a return taking in 24 sends renders a block in 8 us instead of 64 (`INFO routing bus:`), and
  the routing bench's 96 sends cost about +5 to +15% over plain tracks, within the bench's noise now
  (it takes the fastest of three runs of each), against +40% (and +80% at -O3) before.
- **Dynamics without a logarithm a sample**: the Gate compares levels, not decibels; the Compressor
  skips its gain computer under the knee, snaps a released reduction to none, and uses
  `fastDecibels`/`fastDbToLinear` (within 0.001 dB, tested). 24 compressors went from about +55 ms
  over none in the sidechain bench to within its noise.
- **Not changed**: the `scroll` scene (2,000 rows, every one visited each frame; 15 ms when the culling
  refactor measured it, about 4 ms now) is a stress scene of the UI's, not of this work.
