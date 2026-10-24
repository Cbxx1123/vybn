# Freeze and bounce: plan

Status: built, with the differences under "As built".

## Problems

- A track with heavy plugins costs its CPU every block for as long as the project is open, even
  when its part is finished.
- Nothing turns an instrument track, or an audio track with its effects, into audio in the project:
  the only way is an export, then importing the file by hand.

## The model

- `Track::freeze` (`TrackFreeze { file, seconds, hash }`): the file the track's sound was rendered
  into (instrument and effects, before its fader), how long it is, and what it was rendered from.
  `frozen()` when it names a file. Content, like the clips: saved (`[tracks.freeze]` with `file`,
  `seconds`, `hash`), undone ("Freeze track", "Unfreeze track"), compared by the saved-state checks.
- `freezeHash(project, track)`: what the sound depends on: the clips it plays (notes, controllers,
  audio, warps), its instrument and effects (kind, id, values, bypass, automation; not a plugin's
  opaque state), the tempo map, and the content of the tracks its effects are keyed by. A frozen
  track whose hash differs from its freeze's is out of date: it plays live again, and its header
  says so with Refreeze in its menu. So nothing has to forbid editing a frozen track.
- Only audio and instrument tracks freeze (a bus's sound is other tracks').

## Rendering

- `Freezer` (`daw/audio/Freezer.h/.cpp`), owned by DawApp, stepped once a frame: a queue of jobs,
  one rendering at a time, each Freeze, Bounce or Flatten of a track (by id). A job makes an
  `OfflineEngine` over a snapshot holding only the track and the tracks it is keyed by (sends and
  output cleared, master effects dropped), at the device's rate, prepares it on the main thread,
  then runs one `RenderPass` on a Background job: the track tapped before its fader, unmuted, from
  beat 0 to its last clip's end plus a tail that ends once quiet (at most 10 s). The temp file is
  written into a 32-bit float WAV in the project's audio folder (`<project> Audio`, or the
  library's Recordings), "<track> freeze N.wav", with the track's plugin latency cut from its
  start so it lines up. `progress(track)` and `cancel(track)`.
- When it is done (main thread): Freeze sets `Track::freeze` (one undo step); Bounce adds an audio
  track after the source playing the file from beat 0, named "<track> (bounced)", and mutes the
  source (one step, "Bounce track"); Flatten replaces a frozen track by an audio track playing its
  freeze (its effects are in the sound, so they go; its fader, pan, sends, output and volume and pan
  automation stay; "Flatten track").
- Unfreeze clears `Track::freeze` (the file stays on disk, as recordings do).

## Playing

- `AudioBridge`: a track whose freeze is current is made with a clip player as its source playing
  the file from the song's start, and its chain is applied empty, so its instrument and plugins are
  let go (their hosts stop). Its fader, pan, mute, solo, sends, output and volume and pan
  automation work as before; its devices' automation is in the sound. Unfreezing (or the freeze
  going out of date) makes it a live track again, its plugins loaded from its settings. The
  routing plan gives a frozen track no sidechain keys and no plugin latency.
- `renderReady` waits for freeze files, so an export plays them.

## The interface

- A track's menu: Freeze track, Unfreeze track, Refreeze (out of date), Bounce to audio track,
  Flatten (frozen only), Cancel freezing (while it renders).
- The header: a snowflake mark beside the name (accent while current, a warning tone when out of
  date), "Freezing 40%" while it renders; a frozen track's clips are veiled in the arrangement.
- The Devices tab on a frozen track says it is frozen, with Unfreeze.

## Tests

- Export suite: freezing an instrument track writes a file that matches the track's pre-fader
  render; the frozen track then plays the file (its chain empty) and an export of the song is the
  same within -90 dB; editing the track makes the freeze out of date and it plays live; bounce adds
  an audio track and mutes the source; flatten turns it into an audio track; undo puts each back.
- Project suite: the freeze's round trip through the file, the history labels, `freezeHash`
  moving with every input and not with the fader.

## As built

- The header says "Frozen", "Frozen, out of date" or "Freezing 40%" at the start of its instrument or
  input line rather than drawing a mark; a frozen track's clips are veiled and tagged "Frozen".
- The Devices tab's note sits where the latency line goes ("Frozen: these devices are in its sound and
  stopped"); Unfreeze is in the track's menu.
- Flatten of a freeze that is out of date renders first, then flattens (one step when it lands).
- Measured (export suite): a frozen track of noise through a saturation exports within -150 dB of the
  live track; its render ends 0.2 s after its last clip (quiet).
