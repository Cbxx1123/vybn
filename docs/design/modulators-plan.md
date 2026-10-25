# Modulators: plan

Status: built, with the differences under "As built".

## Problems

- A parameter moves only by hand or by automation drawn point by point. A filter sweeping with the
  beat, a tremolo, a pumping volume or one knob turning several parameters at once all need drawing,
  and redrawing when the tempo or the part changes.

## The model

- `audio/Modulation.h`: `ModulatorKind` LFO, Steps, Follower and Macro, each with built-in parameters
  (`modulatorInfo`): LFO shape (sine, triangle, saw up, saw down, square, sample and hold, smooth
  random), rate in Hz or synced (4 bars to 1/32), phase, polarity; Steps 16 step values, the count, a
  synced rate and glide; Follower attack, release and gain; Macro its value.
- `Modulator { id, kind, values, bypass }` and `ModRoute { source, param, depth }` (depth -1 to +1 of
  the knob's travel). `Track::modulators` (at most 8, ids unique in the track) and routes kept on what
  they move: `DeviceSetting::modulation` for a device's parameters (so they move, copy and go with the
  device, as automation does) and `Track::mixModulation` for the track's volume and pan.
- Content for the history ("Add modulator", "Remove modulator", "Modulator settings", "Modulate",
  "Change modulation", "Remove modulation"), the file (`[[tracks.modulators]]`, a device's
  `modulation = [{ source, param, depth }]`, a track's `mix_modulation`), the saved checks and the
  hand-kept Track fields.

## The engine

- `DeviceChain::setModulation(modulators, routes, mixRoutes)` publishes, like automation, the
  modulators (their state kept while their id and kind stay: an envelope's level, a random's draw) and
  each device's routes resolved to its parameter curves. `AudioBridge` makes them when the track's
  revision or its devices' parameter lists move.
- Each block the chain works the modulators out once (at its start), then after a device's automation
  sets each routed parameter to the knob position of its value (its automation's, else its setting's)
  plus depth times the modulator, along the parameter's curve. The track's volume and pan the same, after
  their automation.
- LFO and Steps take their phase from the song's position while it plays (and run on freely while it
  is stopped), so a part sounds the same every time and an export matches; random shapes draw from a
  hash of the cycle. The follower listens to what the track's instrument (or clips) played in the last
  block, before its effects.
- Each modulator's value is published for the interface (an atomic per slot).

## The interface

- The Devices tab: a modulator strip under the cards. Each modulator is a card: its kind, a live
  meter of its value (and the shape for an LFO), its knobs, bypass, remove, and its targets (each a
  name, a depth slider and a remove button) with "Add target" (a menu of the chain's devices and their
  parameters, and the track's volume and pan). "Modulator" adds one (LFO, Steps, Follower, Macro).
- A modulated knob says so in its value ("~").

## Tests

- Audio suite: an LFO's value at known beats for each shape and polarity, synced and free; Steps' values
  and glide; the follower rising and falling at its times; a routed parameter's value over blocks (a
  gain effect's level following a square LFO), depth's sign and clamping, automation under modulation.
- Project suite: the file round trip and refusals, routes moving with a device, the history's labels.

## As built

- A modulator's target list is on its card (depth sliders), not drag-to-assign on the knobs; a modulated
  knob's value ends in "~".
- Modulation is worked out once a block, at its start (about 5 ms at 256 frames), like automation.
- The follower hears what the track played (before its effects) a block late, which its attack hides.
