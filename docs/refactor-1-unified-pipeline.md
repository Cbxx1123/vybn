# Refactor 1: one shared pipeline

Part of `docs/rendering-refactors.md`. Comes after the baseline wave (R0): golden images, off-screen
rendering and frame statistics must exist first.

## Problem

`DrawList` keeps three arrays (`surfaces`, `texts`, `images`) and a list of batches, where a batch is
a run of one kind under one clip. `UIRenderer::render` flattens each batch into its own instance array
and issues one draw per batch, switching pipelines (surface, text, image) and scissor between them.
Interleaved surfaces and text, which is how every control draws, make a batch each, so a frame is
hundreds of draws with a pipeline bind, push constant and descriptor bind in front of most of them.
Text is laid out from strings at submit time, and the renderer allocates new vectors every frame.

## Design

- **One primitive type.** `ui::Prim` (`src/ui/Prim.h`) is the GPU instance for everything drawn: a
  kind (`PrimKind::Surface`, `Glyph`, `Image`; refactor 2 adds `Line` and `Plot`), its rect, its
  clip, and kind-specific fields. `DrawList` becomes an ordered `std::vector<Prim>`: order is
  drawing order, whatever the kind.
- **Text is laid out when it is added.** `DrawList::add(Text)` lays the string out with the
  `Font` (`DrawList::setFont`, set by `Context` each frame) into one `Glyph` prim per visible glyph.
  So the list holds GPU-ready data, which refactor 3 can cache.
- **Clipping per instance.** Each prim carries its clip as whole pixels (`floor` of the start,
  `ceil` of the end, as the scissor was). `prim.vert` clamps the quad to the clip, so pixels outside
  are never rasterised: the same coverage as the scissor, without breaking batches. An unclipped
  prim gets a clip of the whole plane (`kNoClip`, +-1e7).
- **One pipeline, one draw per run.** `shaders/prim.vert` and `shaders/prim.frag` handle every
  kind, branching on a flat `kind` input. Set 0 binding 0 is the font atlas. A frame is one draw of
  all prims, split only around images.
- **Images.** In the MVP images stay on their old pipeline. In Extension they join: the pipeline
  layout gets set 1 (one image's texture), and each image prim is a draw of its own with set 1
  bound to that image (no pipeline change). Images are rare (plugin interfaces), so no descriptor
  indexing is needed and the device stays on Vulkan 1.0.
- **Same picture.** The shader math is copied from `ui.vert`/`ui.frag`/`text.vert`/`text.frag`/
  `image.frag` unchanged. The goldens from R0 must match within the tolerance (each channel within
  2, at most 0.05% of pixels beyond); the MVP keeps colours as floats so it should match exactly.

### Contract: `src/ui/Prim.h` (MVP form)

```cpp
enum class PrimKind : uint32_t { Surface = 0, Glyph = 1, Image = 2, Line = 3, Plot = 4 };

constexpr float kNoClip = 1e7f;

// One GPU instance: matches the inputs of shaders/prim.vert, location = member order, every
// member a vec4 (R32G32B32A32_SFLOAT) except `kind` (R32_UINT at location 7)
struct Prim {
    float rect[4];  // 0: x, y, w, h in framebuffer pixels
    float clip[4];  // 1: x0, y0, x1, y1, whole pixels; nothing outside is drawn
    float shape[4]; // 2: Surface: radius, distance, blur, elevation + rim * 0.45
                    //    Glyph, Image: u0, v0, u1, v1
    float base[4];  // 3: sRGB rgb and opacity (a glyph's colour and alpha; an image's alpha in [3])
    float light[4]; // 4: Surface: light shadow sRGB rgb, intensity
    float dark[4];  // 5: Surface: dark shadow sRGB rgb, intensity
    float style[4]; // 6: Surface: light direction x, y (unit, y down), curvature, form
    uint32_t kind;  // 7: PrimKind
    uint32_t pad[3];
};
static_assert(sizeof(Prim) == 128, "matches prim.vert");

// The clip rect as prims carry it: rounded out to whole pixels; `clipped` false gives kNoClip
void clipBounds(const Rect &clip, bool clipped, float out[4]);
Prim surfacePrim(const Surface &surface, const float clip[4]);
// One Glyph prim per visible glyph of `text`
void glyphPrims(const Font &font, const Text &text, const float clip[4], std::vector<Prim> &out);
Prim imagePrim(const ImageDraw &image, const float clip[4]);
```

`surfacePrim` packs exactly as `toInstance` in today's `UIRenderer.cpp` does (the rim in the
fraction of the elevation, the form in style.w). `glyphPrims` packs as the text path does (rect as
x, y, w, h of the glyph quad; uv in shape; colour in base).

### Contract: `src/ui/DrawList.h` (public API kept, internals replaced)

```cpp
class DrawList {
public:
    void setFont(const Font *font); // text added without a font draws nothing
    void add(const Surface &surface);
    void add(const Text &text);       // laid out now, one Glyph prim per glyph
    void add(const ImageDraw &image); // an Image prim and an entry in images()
    void pushClip(const Rect &rect);
    void popClip();
    void clear();
    bool empty() const;

    const std::vector<Prim> &prims() const;
    struct ImageRef { uint32_t prim; const ImageData *image; };
    const std::vector<ImageRef> &images() const;
};
// Hashes every prim's bytes in order, then each image's id and version
uint64_t fingerprint(const DrawList &list, uint64_t start = Fingerprint::kOffset);
```

`Fingerprint` stays as it is (other code may use it). Nothing outside `DrawList.*`, `UIRenderer.*`,
`Context.cpp` and the tests reads the list's internals (checked: only `tests/InputSelfTest.cpp`
builds a `DrawList` directly), so the ~80 component classes don't change.

### Shader interface: `shaders/prim.vert` / `prim.frag`

- Push constant: `vec2 screenSize` (as today).
- Inputs: locations 0-6 `vec4`, location 7 `uint kind`, all per instance; six vertices per quad.
- The vertex shader builds the quad (a surface's quad grown by its shadow margin as `ui.vert` does;
  a glyph's or image's quad is its rect), clamps it to the clip, and passes the local position
  (surface), the uv recomputed linearly from the clamped position (glyph, image), and every input as
  a flat varying.
- The fragment shader branches on `kind`: Surface is `ui.frag`'s body, Glyph is `text.frag`'s, Image
  is `image.frag`'s. A surface's distance derivatives (`dFdx`, `dFdy`) are taken outside the branch
  (compute the distance for every kind, 0 for non-surfaces, then the derivatives), as `ui.frag` does.

## Stages

### R1.0 Contracts (orchestrator)

`src/ui/Prim.h`, the new `src/ui/DrawList.h` (inline code moved to a new `src/ui/DrawList.cpp`),
`shaders/prim.vert` and `prim.frag` as stubs listing their inputs, `CMakeLists.txt` (`DrawList.cpp`,
`Prim.cpp`, the two shaders in `SHADER_SOURCES`).

### R1.1 MVP: surfaces and text through one pipeline

| Agent | Owns | Task |
|---|---|---|
| D prims | `src/ui/Prim.cpp`, `src/ui/DrawList.cpp` | `clipBounds`, `surfacePrim`, `glyphPrims`, `imagePrim`; `DrawList` as documented; `fingerprint` over prims and images |
| E shaders | `shaders/prim.vert`, `shaders/prim.frag` | Surface and Glyph exactly as the old shaders; Image path too (used from Extension) |
| F renderer | `src/ui/UIRenderer.h` (private members only), `src/ui/UIRenderer.cpp`, `src/ui/Context.cpp` (`setFont` before drawing) | one prim pipeline (set 0 = atlas); upload all prims; one `drawInstanced` per run of non-image prims; image prims drawn by the old image pipeline with a scissor from their clip; stats as in R0 |
| G tests | `tests/InputSelfTest.cpp` (fingerprint checks), `tests/RenderSelfTest.cpp` (prim checks) | the fingerprint checks with a font set; new checks: clip rounding, a clipped glyph's uv, prim order across kinds, an empty text adding nothing |

**Done when:** all self-tests pass; `--render-selftest` matches every golden; frame stats show the
draws per frame of `--demo transport` falling from hundreds to under 10.

### R1.2 Extension: images join; the old path goes

| Agent | Owns | Task |
|---|---|---|
| H images | `src/ui/UIRenderer.cpp`, `shaders/prim.frag` | the prim pipeline layout gets set 1 (an image); each image prim is one draw with its set bound; the live image textures as today; the old image pipeline removed |
| I engine | `src/engine/Pipeline.h/.cpp`, `src/engine/Renderer.h/.cpp` (`bindDescriptorSet` only) | `PipelineOptions::descriptorSetLayouts` (a list: set 0, set 1, ...) replacing the single pointer; `Renderer::bindDescriptorSet(pipeline, set, index = 0)` |
| J metrics | `src/daw/DawAudio.cpp` (`buildMetrics` only) | a "UI drawing" line in Settings > Metrics: prims, glyphs, draws and bytes a frame, build and submit ms |
| K cleanup | `shaders/ui.vert`, `ui.frag`, `text.vert`, `text.frag`, `image.frag` (deleted) | after H and I land; the orchestrator removes them from `SHADER_SOURCES` |

The contract for I (the two engine signatures) is written by the orchestrator first so H can build
against it in parallel. **Done when:** no code uses the old pipelines or `DrawList`'s old batches;
goldens match (including the image scene); the `--demo plugineditor` interface still draws (checked
by the orchestrator with `--plugin-folder output/plugins`).

### R1.3 Performance

First the orchestrator records the numbers (R0's baseline and after R1.2) for the four demos in this
file. Then:

| Agent | Owns | Task |
|---|---|---|
| L packing | `src/ui/Prim.h` (layout only, from the orchestrator's new contract), `src/ui/Prim.cpp`, `src/ui/UIRenderer.cpp` (vertex layout) | base, light and dark colours as RGBA8 `uint32_t`s (light.a and dark.a the intensity); `Prim` down to 80 bytes: rect, clip, shape, style, and a `uvec4` of base, light, dark, kind |
| M shader | `shaders/prim.vert` | unpack the colours (`unpackUnorm4x8`); everything else unchanged |
| N kerning | `src/ui/Font.cpp` | a kerning table for the 95 printable ASCII characters (95 x 95 in font units, built once when the font loads) used by `measure` and `layout` instead of `stbtt_GetCodepointKernAdvance`; results identical |
| O culling | `src/ui/DrawList.cpp`, `src/ui/DrawList.h` (private members) | a prim whose drawn bounds (a raised surface's grown by its shadow margin, as `prim.vert` does) miss its clip is not added; vectors keep their capacity between frames; `reserve` from the last frame's size |
| P bench | `tests/RenderSelfTest.cpp` | `INFO render:` per scene: build ms, prims, bytes, draws, submit ms; a 2000-row `ScrollView` scene showing culling |

Targets (against the baseline): draw calls per frame under 10 in every demo; bytes uploaded per
frame down by at least 30%; build time per frame of `--demo transport` no worse than the baseline
(text layout moved from submit to build must be paid for by kerning and culling); goldens within
tolerance after packing (8-bit colours may move a channel by 1).

### R1.4 Polish

| Agent | Owns | Task |
|---|---|---|
| Q docs | `CLAUDE.md` | the UIRenderer, DrawList, Prim and shader paragraphs rewritten; the self-test listed; build notes for the new shaders |
| R review | files the orchestrator names | fixes from the orchestrator's review (comments, names, dead code) |

The orchestrator runs every demo with `VIBIN_FRAME_STATS=1`, looks at screenshots of `menu`,
`settings`, `synth`, `pianoroll`, `steps`, `effects`, `browser` and `export`, and commits.

## Risks

- **The look shifts.** Guarded by goldens; colours stay float until Performance, where packing is
  checked against the same goldens.
- **Derivatives in branches.** Taken before the kind branch, as the old shader did.
- **Bigger glyph instances** (48 bytes to 128, then 80). More than paid for by a few draws instead of
  hundreds and no per-frame vectors; measured in Performance.
- **Text laid out in build, not submit,** costs CPU on frames that are then skipped as unchanged.
  Kerning tables and culling pay for it; refactor 3's caching removes most of it.

## Out of scope

Unicode text, display scale, GPU timestamps, multi-window submission in one queue submit.
