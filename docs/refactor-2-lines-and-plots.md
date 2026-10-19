# Refactor 2: lines, curves and waveforms

Part of `docs/rendering-refactors.md`. Builds on refactor 1: new kinds in the `Prim` stream, drawn by
`prim.vert`/`prim.frag`.

## Problem

The draw list knows rounded rectangles and glyphs, so every curve in the app is built from columns of
tiny rectangles, each a full neumorphic surface (a signed-distance shape with shadow paths):

| Where | How it draws today | Prims |
|---|---|---|
| `SynthView.cpp` `drawCycle`, `FilterCurve`, `EnvelopeDisplay`, `LfoDisplay`, `SubDisplay` | two 2 px rects per 2 px column (a soft fill and a "line") | ~300 per display, ~1,500 for the synth page |
| `Arrangement.cpp` audio clips | one 1.5 px rect per 2 px of visible waveform | thousands across a project |
| `Arrangement.cpp` fades | a dot every 5 px | dozens |
| `AudioEditor.cpp` waveform | one rect per 2 px | ~600 at 1200 px |
| `PianoRoll.cpp` controller lane | two rects per 2 px column | ~600 |
| `ExportsPanel.cpp` loudness graph | one rect per point | one per 100 ms of audio |

The lines are stair-stepped (a 2 px column is drawn flat), a slope reads as steps, and the counts
make these views the heaviest in the app to build, hash and upload.

## Design

Two new primitives, both analytic (a signed distance in the fragment shader, as surfaces are, so
edges anti-alias the same way at any zoom):

- **`Line`**: a segment from `a` to `b`, `width` pixels thick, round caps, one colour and opacity.
  `DrawList::addPolyline(points, count, style)` adds a run of segments. Use for fades, guides, the
  envelope's straight stages, anything with unevenly spaced points.
- **`Plot`**: one rectangle whose shape comes from a list of values the shader reads, so a whole
  curve or waveform is one prim:
  - `PlotMode::Line`: the values evenly spaced across the rect, joined by a line `width` thick.
  - `PlotMode::Fill`: the area between the curve and `baseline`.
  - `PlotMode::Range`: `count` columns of `low, high` pairs (a waveform's min and max), each column
    `rect.w / count` wide with a bar `barFraction` of it (1.5 of 2 px keeps today's look).
  The values are copied into the draw list's data (`DrawList::plotData()`) when the plot is added, so
  the caller's buffer only needs to live through the call. `low` and `high` map values to the rect's
  bottom and top.

A curve with a soft fill under a bright line, as the synth and the piano roll's lanes draw, becomes
two plots (Fill at low opacity, Line on top) instead of hundreds of rectangles.

### Contract: drawables (`src/ui/Theme.h`)

```cpp
// A straight stroke with round caps, the third drawable primitive
struct Line {
    Vec2 a, b;
    float width = 1.0f;
    Color color;
    float opacity = 1.0f;
};

enum class PlotMode { Line, Fill, Range };

// A curve, filled curve or waveform drawn from values in one primitive
struct Plot {
    Rect rect;
    PlotMode mode = PlotMode::Line;
    const float *values = nullptr; // count values; Range: count pairs of low, high
    uint32_t count = 0;
    float low = -1.0f, high = 1.0f; // the values at the rect's bottom and top
    float baseline = 0.0f;          // Fill: filled between the curve and this value
    float width = 2.0f;             // Line: the stroke's width in pixels
    float barFraction = 1.0f;       // Range: the share of each column the bar fills
    Color color;
    float opacity = 1.0f;
};
```

`Theme` gets the colour helpers (components still never pick colours):
`Line Theme::inkLine(Vec2 a, Vec2 b, float width, TextTone tone = TextTone::Primary, float opacity = 1)`,
`Line Theme::accentLine(Vec2 a, Vec2 b, float width, float opacity = 1)`,
`Plot Theme::inkPlot(const Rect &rect, PlotMode mode, TextTone tone = TextTone::Secondary, float opacity = 1)`,
`Plot Theme::accentPlot(const Rect &rect, PlotMode mode, float opacity = 1)` (the caller then sets
`values`, `count`, `low`, `high` and the mode's fields).

### Contract: `DrawList` and `Prim`

```cpp
void add(const Line &line);
void addPolyline(const Vec2 *points, size_t count, const Line &style); // style's a and b ignored
void add(const Plot &plot);
const std::vector<float> &plotData() const; // every plot's values, in the order added
```

`Prim` for a `Line` (kind 3): rect = the capsule's bounds grown by 1 px; shape = a.x, a.y, b.x, b.y;
style.x = half the width; base = colour and opacity. For a `Plot` (kind 4): rect = the plot's rect;
shape = first value's index into `plotData()`, count, low, high (as floats: exact below 2^24);
style = mode, width or barFraction, baseline, 0; base = colour and opacity. The fingerprint hashes
`plotData()` too.

### GPU

- Set 0 becomes per window and per frame in flight: binding 0 the font atlas (as now), binding 1 a
  storage buffer `readonly buffer PlotData { float values[]; }`. The UIRenderer keeps one data
  buffer per frame in flight, grown on demand like the instance buffers, and rewrites that frame's
  set when its buffer is replaced (safe after `beginFrame`).
- Set 1 stays the image. Every draw binds something there: a 1x1 texture when there is no image.
- `prim.frag`: Line is the capsule's distance; Plot Line is the distance to the nearest of the three
  segments around the pixel's x; Plot Fill is inside between the curve and the baseline, with the
  curve's edge anti-aliased by its distance; Plot Range is the pixel's column's bar, anti-aliased on
  its four sides. Colours are linearised like every other kind.
- Engine: `DescriptorBinding` takes `VK_DESCRIPTOR_TYPE_STORAGE_BUFFER`;
  `Renderer::createDescriptorPool(maxSets, maxTextures, maxBuffers = 0)`;
  `DescriptorSet::writeBuffer(binding, const Buffer &)`; `Renderer::createBuffer` usage may include
  `VK_BUFFER_USAGE_STORAGE_BUFFER_BIT`. All Vulkan 1.0.

## Stages

### R2.0 Contracts (orchestrator)

The structs and helpers above in `Theme.h`, `DrawList.h` and `Prim.h`, the engine signatures in
`Descriptors.h`/`Renderer.h`, and `prim.frag`'s new inputs listed in its header comment.

### R2.1 MVP: lines

| Agent | Owns | Task |
|---|---|---|
| S lines | `src/ui/Prim.cpp`, `src/ui/DrawList.cpp`, `src/ui/Theme.cpp` | `linePrim`, `DrawList::add(Line)`, `addPolyline`, culling a line like other prims, the theme's line helpers |
| T shader | `shaders/prim.vert`, `shaders/prim.frag` | the Line kind: a quad over its bounds, clamped to the clip; the capsule's distance; coverage as surfaces anti-alias |
| U tests | `tests/RenderSelfTest.cpp` | a lines scene (widths 1, 1.5, 2, 3 and 6 at 0, 15, 45 and 90 degrees, opacity 0.3 and 1, clipped) and its golden; checks of `addPolyline`'s segment count and culling |
| V synth | `src/daw/SynthView.cpp` | `EnvelopeDisplay` (its stages as a polyline) and the cutoff line as `Line`s; nothing else in this wave |

**Done when:** tests pass; the new golden looks right to the orchestrator (crisp, even width at every
angle); the envelope reads as one smooth line in `--demo synth`.

### R2.2 Extension: plots, and every curve moved onto them

Two waves: the primitive, then its users.

Wave a, the primitive:

| Agent | Owns | Task |
|---|---|---|
| W engine | `src/engine/Descriptors.h/.cpp`, `src/engine/Buffer.cpp`, `src/engine/Renderer.cpp` (the pool) | storage-buffer descriptors as in the contract |
| X plots | `src/ui/Prim.cpp`, `src/ui/DrawList.cpp`, `src/ui/Theme.cpp`, `src/ui/UIRenderer.cpp` | `plotPrim`, `DrawList::add(Plot)` and `plotData()`, the theme's plot helpers; the per-window set 0, the data buffers and the dummy image |
| Y shader | `shaders/prim.frag` | the three plot modes |
| Z tests | `tests/RenderSelfTest.cpp` | a plots scene (a sine as Line, Fill to the middle, a noise waveform as Range at bar 0.75, a clipped plot, a 2-value plot, a 4096-value plot in 300 px) and its golden; data offsets across several plots |

Wave b, the users (each agent one file):

| Agent | Owns | Task |
|---|---|---|
| AA arrangement | `src/daw/Arrangement.cpp` | audio clips as one Range plot each (one column per 2 px, bar 0.75, as today); fades as polylines |
| AB audio editor | `src/daw/AudioEditor.cpp` | the waveform as two Range plots (inside the clip and the dimmed rest); the fade curves as polylines |
| AC synth | `src/daw/SynthView.cpp` | `drawCycle`, `FilterCurve`, `LfoDisplay`, `SubDisplay` as Fill plus Line plots |
| AD piano roll | `src/daw/PianoRoll.cpp` | the controller lane as a Fill plot and a Line plot (stepped controllers sampled so steps stay vertical) |
| AE exports | `src/daw/ExportsPanel.cpp` | the loudness graph as a Line plot over a Fill plot |

**Done when:** no column-of-rects curve is left (the `for (float x ...; x += 2.0f)` loops above are
gone); tests pass; the DAW goldens' changes are reviewed by the orchestrator and updated; `--demo
synth`, `--demo audioclip`, `--demo pianoroll` (with a lane showing) and `--demo exportmix` look right.

### R2.3 Performance

The orchestrator records prims and build ms for `--demo synth`, `--demo audioclip` and a project with
eight long audio clips before and after.

| Agent | Owns | Task |
|---|---|---|
| AF waveform cache | `src/daw/Arrangement.cpp`, `src/daw/Arrangement.h` (private members) | each audio clip's column values kept between frames, keyed by the clip's id, its data, zoom, scroll and gain, so `AudioData::range` runs only when one changes |
| AG audio editor cache | `src/daw/AudioEditor.cpp`, `src/daw/AudioEditor.h` (private members) | the same for the editor's waveform |
| AH joints | `src/ui/DrawList.cpp` | `addPolyline` with translucent colours: no double blending where segments meet (each segment after the first starts its capsule at the joint with a flat cap, or the polyline becomes one prim with its points in the plot data; the agent picks one, the orchestrator reviews) |
| AI data buffer | `src/ui/UIRenderer.cpp` | the plot data written straight into the mapped buffer (no copy through a vector); only the frame's used range |

Targets: the synth page under 150 prims for its displays (from about 1,500); an audio clip one prim
whatever its length; build ms for the eight-clip project down by at least half; no stair steps.

### R2.4 Polish

| Agent | Owns | Task |
|---|---|---|
| AJ docs | `CLAUDE.md` | `Line`, `Plot`, the theme's helpers, the data buffer, the self-test scenes; the views' paragraphs updated |
| AK review | files the orchestrator names | widths and opacities tuned to read like before (the orchestrator compares screenshots of each view before and after), comments, dead helpers gone |

## Risks

- **The look changes on purpose.** Curves become smooth lines; the orchestrator compares every
  touched view before and after and keeps the weights (line width, fill opacity) close to today's.
- **Range plots at extreme zoom.** One column per 2 px is the caller's choice, as now; the shader
  only draws what it is given, so a zoomed-out clip is still one prim.
- **Storage buffers** are core Vulkan 1.0, and fragment shaders may read them on every device the
  app runs on today.

## Out of scope

Bezier paths, filled polygons, gradients, dashed lines (each would be another kind later).

## Results (19 October 2026)

| | Before refactor 2 | After |
|---|---|---|
| synth panel (render self-test): prims | 4,688 | 1,574 |
| synth panel: upload | 366 KB | 135 KB |
| synth panel: build (50-frame average) | 1.92 ms | 1.32 ms |
| eight 30 s audio clips in the arrangement: build | (one rect per 2 px) | 0.61 ms, 232 prims |

The synth's remaining prims are mostly its knobs' rings of dots, which are not curves. No curve is
drawn as columns of rects any more: waveforms are Range plots, every other curve a Line plot
(with a Fill plot under it where it was filled), and straight guides Lines.

Decisions in the Performance stage:
- The waveform caches (agents AF and AG) were not built: eight long clips cost 0.61 ms to build
  in all, and refactor 3 caches the arrangement's and audio editor's whole drawing anyway.
- Joints: every translucent polyline in the app was a curve sampled at even x, so each became one
  Line plot (no joints at all) instead of changing how polylines join. `addPolyline` stays for
  opaque strokes and uneven points.
- The plot data is copied once, from the DrawList's vector into the mapped buffer (agent AI's
  item); a second path to skip that copy wasn't worth its complexity at these sizes.
