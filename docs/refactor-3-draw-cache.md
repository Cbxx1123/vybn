# Refactor 3: cached drawing for heavy views

Part of `docs/rendering-refactors.md`. Builds on refactors 1 and 2: the draw list holds GPU-ready
prims (text already laid out) and plot data, so a section of it can be kept and appended again.

## Problem

Each tick, for every visible window:

1. `Context::update()` lays out the whole tree (`layout()`), then runs input.
2. `Context::buildFrame()` lays it out again (in case input changed it), calls `draw` on the whole
   tree, and hashes the whole list for the fingerprint.

When nothing on screen changed the GPU work is skipped, but all of the above still runs. During
playback something changes every frame (meters, the playhead), so the whole tree is laid out twice
and redrawn 60 times a second, though the arrangement's clips, the piano roll's notes and the synth's
displays didn't move.

## Design

### One layout per frame

`buildFrame` always lays out (input and callbacks may have changed anything). `update` lays out only
when the rects from the last `buildFrame` can't be used for this frame's input:

- the tree's structure changed: `Component::treeVersion()`, a counter bumped by `Container::add`,
  `insert`, `remove`, `clear`, `adopt` and `release` and by `ScrollView::setContent`/`takeContent`;
- an overlay opened or closed, or the window's size changed;
- the theme changed (`Theme::fingerprint()`, a hash of every value, compared with the last layout's);
- it is the first frame.

Otherwise input is hit-tested against the rects of the frame on screen, which is what the user is
pointing at. A state change made between frames (a posted action, a page shown) is laid out by the
next `buildFrame`, as now.

### Draw caches

```cpp
// Keeps what a component drew while what it depends on stays the same (a repaint boundary, as in
// Flutter or Chromium's paint chunks). Held by the component as a member.
class DrawCache {
public:
    explicit DrawCache(const char *name); // for the verifier's messages and the stats

    // Runs `paint` (which adds to `list`), or, when `key` and the parts added for it (the list's
    // clip, `rect`, the theme's fingerprint and the font) are what they were last time, appends
    // what it added then. `key` must cover everything `paint` reads. `reuse` false always paints
    // (while recording, while something animates). A section holding an image is never reused.
    template <typename Paint>
    void draw(DrawList &list, const Theme &theme, const Rect &rect, uint64_t key, Paint &&paint,
              bool reuse = true) const;

    void invalidate() const;
    uint64_t hits() const;
    uint64_t misses() const;
};
```

`DrawList` gets sections:

```cpp
// What was added between beginSection and endSection: its prims, plot data and images, and its
// hash. Sections nest.
struct DrawSection {
    std::vector<Prim> prims;
    std::vector<float> data;  // plot values, offsets relative to the section
    bool hasImages = false;
    uint64_t hash = 0;
};
void beginSection();
void endSection(DrawSection &out);
void appendSection(const DrawSection &section); // rebases plot offsets; folds in its hash
```

**The fingerprint becomes incremental.** The list keeps a running hash as things are added (each
prim's bytes, each plot's values). A section is folded into its parent as one item, its own hash,
whether it was painted now or appended from the cache, so a cached frame and a painted one with the
same content give the same fingerprint. `fingerprint(list)` then only adds the images' ids and
versions, so it no longer walks the whole list.

**The verifier.** With `VIBIN_VERIFY_CACHE=1`, every `DrawCache::draw` paints; when the key matched,
it compares what was painted with the cached section, and the first time they differ it prints
`DrawCache: <name> drew something else under the same key` to stderr. That is how a key missing an
input is found, the same way `VIBIN_VERIFY_STORE` finds edits made outside the store.

### Which views, and how they split

A view draws its cached part through its `DrawCache`, then what changes every frame on top. The
order of what is drawn must stay the same as today (goldens check it).

| View | Cached (key from) | Drawn every frame |
|---|---|---|
| `ArrangementView` | ruler, loop band, lanes, grid, clips with their notes and waveforms (store revision, selection revision, scroll, zoom, size, hovered clip and edge, trim state, the audio pool's version; no reuse while recording) | playhead, drop target |
| `PianoRollView` | grid, row shading, notes, ghosts, lane content, overview (store revision, selection, scroll, zoom, row height, fold, scale, tool, hover row and note, lane) | playhead, keys lit while playing, drag previews, lasso, slice line, readout |
| `StepSequencerView` | names, header, grid, lane (store revision, view settings, rows, selected row, scroll, hover) | the playing step |
| `AudioEditorView` | waveform, fades, ruler (store revision, data, scroll, zoom, hover handle) | playhead |
| `SynthView` displays | each display (the values it reads, its drag state) | nothing |
| `FileDialog`'s `FileList` | the rows (the listing's version, sort, filter, selection, scroll, hover) | nothing |
| `ExportsPanel`'s `LoudnessGraph` | the graph (its series never changes) | nothing |

## Stages

### R3.0 Contracts (orchestrator)

`src/ui/DrawCache.h`, the section API and running hash in `DrawList.h`, `Component::treeVersion()` in
`Component.h`, `Theme::fingerprint()` in `Theme.h`, `FrameStats` fields `cacheHits`, `cacheMisses`,
`cachedPrims` and `layoutsSkipped`.

### R3.1 MVP: one layout, sections, the cache, and the arrangement

| Agent | Owns | Task |
|---|---|---|
| AL sections | `src/ui/DrawList.cpp` | the running hash, sections (nesting, plot offsets rebased, images flagged), `fingerprint` from the running hash |
| AM cache | `src/ui/DrawCache.cpp` (any non-template parts), `src/ui/Theme.cpp` (`fingerprint`) | the cache, the verifier, the stats counters |
| AN layout | `src/ui/Layout.cpp`, `src/ui/Navigation.cpp` (`setContent`/`takeContent`), `src/ui/Context.cpp` | `treeVersion` bumps; `update` lays out only when needed; `layoutsSkipped` counted |
| AO arrangement | `src/daw/Arrangement.h/.cpp` | the split and key from the table |
| AP tests | `tests/RenderSelfTest.cpp` | a section appended equals the painted one (prims, data, hash); a changed key repaints; a nested section; plot offsets after appending; a section with an image not reused; the verifier catching a key missing an input; the arrangement scene painted twice gives one hit and the same prims |

**Done when:** all self-tests and goldens pass; `--demo transport` with `VIBIN_VERIFY_CACHE=1` for 30
seconds prints nothing; frame stats show the arrangement hitting its cache while the transport plays
and `update` skipping its layout on most frames.

### R3.2 Extension: the other heavy views

One agent per file: AQ `PianoRoll.h/.cpp`, AR `StepSequencer.h/.cpp`, AS `AudioEditor.h/.cpp`,
AT `SynthView.cpp`, AU `FileDialog.cpp` (`FileList`), AV `ExportsPanel.cpp` (`LoudnessGraph`), each
with the split and key from the table, then AW tests (`tests/RenderSelfTest.cpp`: each view's scene
painted twice hits its cache and draws the same).

**Done when:** every demo run for 20 seconds with `VIBIN_VERIFY_CACHE=1` and some interaction the
demo performs itself prints nothing; tests and goldens pass.

### R3.3 Performance

The orchestrator records, before and after, `--demo transport`, `--demo pianoroll` (playing),
`--demo steps` (playing), `--demo synth` and `--demo open` (the file dialog on a big folder):
update, build and submit ms, prims, cache hits and misses, layouts skipped.

| Agent | Owns | Task |
|---|---|---|
| AX culling | `src/ui/Layout.cpp` (`Container::draw`), `src/ui/DrawList.h/.cpp` (a `clip()` getter) | a child whose rect, grown by 32 px, misses the list's clip isn't drawn at all (long settings pages, the browser's rows, the shortcuts list) |
| AY keys | the view files the numbers point at | keys that miss too often (hover changes that don't change the picture, glides) tightened, measured |
| AZ stats | `src/ui/UISystem.cpp`, `src/daw/DawAudio.cpp` (`buildMetrics`) | cache hits, misses and layouts skipped in the `VIBIN_FRAME_STATS` line and the Metrics page |

Targets: build ms during playback down by at least 40% in `--demo transport` and `--demo pianoroll`;
`update` lays out on under 10% of frames when the mouse is still; no verifier output.

### R3.4 Polish

| Agent | Owns | Task |
|---|---|---|
| BA docs | `CLAUDE.md` | `DrawCache`, sections, the one-layout rule (what bumps `treeVersion`), the verifier, which views cache and their keys |
| BB review | files the orchestrator names | fixes from review and from a last verifier run over every demo |

## Risks

- **A key that misses an input shows a stale view.** Only the view that owns the cache can be stale,
  the verifier finds it, and goldens check the first paint. Views pass `reuse = false` whenever they
  are unsure (recording, gliding).
- **Hit-testing against last frame's rects** after a state change made between frames: that is what
  the screen shows, so it is what the user aims at. A structural change still lays out first.
- **Copying sections** costs a memcpy of their prims. Far cheaper than painting them (text layout,
  waveform ranges), and measured in Performance.

## Out of scope

Caching for composed components (the mixer's strips, track headers, device cards: their children
draw themselves and have no key), off-screen layers (pixels kept on the GPU), skipping `update`
entirely for quiet windows.
