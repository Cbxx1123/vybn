# VST3 plugins: plan

Status: built, and the items once listed as not done (see the end of "As built"), all but note
expression. The plugin suite runs the VST3 checks (132 checks in all, about 4 s).

## Goal

VST3 plugins work wherever CLAP plugins do (effects, instruments, sidechains, automation, their
own interfaces, saved state, the sandbox), and the user never has to know which format a plugin
is: one list of plugins, one set of folders, one way of adding them.

## Interfaces

- Steinberg's `vst3_pluginterfaces` (MIT since 3.8), fetched at `v3.8.0_build_66` like CLAP, as
  headers only: each interface's id is an inline constant (`IComponent_iid`), compared byte for
  byte, so no SDK source is compiled and nothing of the SDK's helper library is used.
- MinGW builds them as it builds COM: the interfaces are plain pure virtual classes with no
  destructors, laid out the same as by MSVC.

## The host program

`vybn-plugin-host.exe` hosts either format, chosen by the file it is given (`.clap` or `.vst3`).
Its parts that aren't about a format stay shared: the pipes and messages, the shared memory and
the audio thread's wait, the sandbox, and the embedded interface's frame window, its capture and
input. A **format** is the rest, behind one interface in the host:

- `scan(path)` -> descriptors; `probe(id)` -> sidechain channels; `load(id)` -> `LoadedInfo`;
  `activate(rate, frames)`, `deactivate()`; `process(block)`; `params()`, `paramText(id, value)`;
  `saveState()`, `loadState(bytes)`; `guiAttach(frame window)`, `guiSize`, `guiResize`,
  `guiDetach`; `guiOpenFloating`, `guiCloseFloating`; housekeeping on the main thread.
- `ClapFormat`: the code there is now, moved behind it.
- `Vst3Format`:
  - Loading: a `.vst3` bundle (`Contents/x86_64-win/<name>.vst3`) or a single-file `.vst3`;
    `InitDll`, `GetPluginFactory`, `ExitDll`. The factory's "Audio Module Class" classes are the
    plugins; each id is `vst3:` and its class id in hex, so it never meets a CLAP id.
  - What it is: the class's subcategories ("Instrument|Synth", "Fx|Delay") become the same
    feature words CLAP uses ("instrument synthesizer", "audio-effect delay"), so everything that
    asks whether a plugin is an instrument or an effect keeps working.
  - Running it: the component and its edit controller (one object, or two connected through
    `IConnectionPoint`), the controller given the component's state; buses: the main stereo in
    and out, the event input of an instrument, and an aux input as the sidechain;
    `setupProcessing` (32-bit, realtime), `setActive`, `setProcessing`; `process` with the block's
    audio, an `IEventList` of notes, `IParameterChanges` of the app's changes at their frames,
    and a `ProcessContext` from the transport (tempo, signature, bar, playing).
  - Parameters: shown as CLAP's are. A stepped parameter (`stepCount` > 0) as its steps (0 to
    stepCount), a continuous one as 0-1; names, units in the name's module, flags (hidden,
    read-only, bypass, list). Texts from the controller (`getParamStringByValue`). MIDI
    controllers and the bend through `IMidiMapping` to the parameters they drive.
  - Changes the plugin makes (its interface's `performEdit`, the processor's output parameter
    changes) go back to the app through the same ring as CLAP's, and to the other half of the
    plugin; `restartComponent` (latency, parameter lists) becomes the same news CLAP's sends.
  - State: the component's and the controller's, both, in one blob of our own (a tag, two
    lengths), restored component first.
  - Interface: `createView("editor")` attached to the frame window (`HWND`), sized by
    `IPlugFrame::resizeView` and `onSize`, the same embedded and floating ways as CLAP.

## The app

- `Descriptor` gets the plugin's `format` (clap, vst3) and its file; nothing on screen names it
  except the plugin's details in Settings > Plugins.
- One scan: the plugin folders hold either format (`.clap` files, `.vst3` bundles or files); the
  standard folders are the CLAP and VST3 ones (`%COMMONPROGRAMFILES%\CLAP`, `\VST3`, and the
  per-user ones). The scan cache keeps the format. Import plugin takes either.
- A plugin installed in both formats is listed once (matched by name and vendor), per the choice
  below; the other stays reachable for projects that use it.
- Projects save the plugin's id as now; a VST3 plugin's id is its own, so nothing old changes.
- Wording: "CLAP plugin" becomes "plugin" everywhere it is shown.

## Tests

- A second test plugin module, `vybn-test-plugins.vst3` (a gain effect with a parameter, a sine
  instrument and an effect with a sidechain input), written against the interfaces alone,
  built into `output/plugins`, and run through the plugin suite as the CLAP ones are: scanning,
  loading, processing, parameters and their texts, MIDI controllers, state round trips, latency,
  the sidechain, a crash.
- A project with a VST3 effect and instrument through the bridge.

## As built

- The host is `host/HostMain.cpp` (either format's), `host/Host.h` (the `Format` interface and the
  news), `host/ClapFormat.cpp` (the CLAP code, moved) and `host/Vst3Format.cpp`. The protocol is
  unchanged (still version 4): the app sends the same requests whichever format a plugin is.
- `Descriptor` has no saved format: `format()` reads it from the path (`isVst3Path`), so the scan
  cache and projects needed no change. The plugin's details in Settings > Plugins end with "CLAP"
  or "VST3", the only place it shows.
- A bundle's scan stamp is its module's size and time, so a changed plugin is scanned again.
- Import plugin takes `.clap` and `.vst3`; a module picked inside a bundle copies the whole bundle.
- A plugin in both formats: the VST3 one is kept aside in the manager (`others_`), found by id
  and by `sidechainChannels`, not listed.
- Floating editors: VST3 has none of its own, so the host makes a window (`VybnPluginWindow`) and
  attaches the editor to it; closing it is the same news as a CLAP window closing.
- An edit made in a VST3 editor while the plugin is inactive reaches the app at once and its
  processor in its first block after activation.
- Units: a parameter's unit and those above it are its `module` ("Voice/Pitch"); the root unit names
  nothing. A program-change parameter whose unit has a program list is a list (stepped, an enum) of the
  programs, its steps from the list when it gives none, its texts the programs' names.
- `IComponentHandler2`: `setDirty` is the state-dirty news CLAP's `state` sends; group edits are
  accepted and their edits arrive one by one; `requestOpenEditor` is refused (the app opens editors).
  `IUnitHandler`'s program list changes rescan the parameters' texts and values.
- A plugin that processes only doubles gets them: double buffers for every bus, converted around
  `process`.
- Each note's id is its channel and key, on its note-on and note-off.
- A bundle's `Contents/Resources/moduleinfo.json` is read by the app (`plugin/ModuleInfo.cpp`, JSON5)
  and lists its plugins without a host; without one, or with one that can't be read, the bundle is
  scanned in a host as before. The class ids are taken as the file writes them, the TUID's bytes in
  order. The test bundle carries one (`tests/plugins/moduleinfo.json`), checked against its factory.
- Note expression: built with per-note expression in the app (docs/design/note-expression-plan.md).
  The host reads the plugin's `INoteExpressionController` (its first event bus, channel 0) and, if it
  lists `kTuningTypeID`, sends a sounding note's tuning as `kNoteExpressionValueEvent`s against the
  note's id (240 semitones across 0-1, within the range the plugin gives); a plugin that lists none
  gets none. The app's other expressions (pressure, timbre, volume, pan) are for later.
