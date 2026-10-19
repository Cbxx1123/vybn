# Rendering refactors: overview

Three refactors of the rendering system, chosen after an audit (October 2026), built by Haiku 5.5
agents in waves. Each refactor has its own plan:

| # | Refactor | Direction chosen | Plan |
|---|---|---|---|
| 1 | GPU renderer | One shared pipeline: surfaces, text and images as one instance type, clipped per instance | `docs/refactor-1-unified-pipeline.md` |
| 2 | Lines, curves and waveforms | Shader-drawn lines, polylines and plots | `docs/refactor-2-lines-and-plots.md` |
| 3 | Frame pipeline | Cached drawing for heavy views, and one layout per frame | `docs/refactor-3-draw-cache.md` |

They run in this order: 2 adds new kinds to the shared primitive stream from 1, and 3 caches
sections of the draw list in its final format.

## What the audit found

- **Every frame redoes everything.** `UISystem::tick` calls `Context::update()` (which lays out the
  whole tree) and then `Context::buildFrame()` (which lays it out again, rebuilds the whole
  `DrawList` and hashes every field of it), for every visible window. Only then does it decide the
  frame is unchanged and skip the GPU work. During playback the meters change every frame, so all
  of it runs at 60 Hz.
- **Hundreds of draw calls.** `DrawList` starts a new batch whenever the kind changes (surface,
  text, image) or the clip changes, and `UIRenderer` re-binds the pipeline, push constants and
  descriptor set for each. A button is a surface then text, so a row of 10 buttons makes 20 batches.
  Every image is a draw of its own, and an image changing size calls `waitIdle`.
- **Heavy records.** A `Surface` is about 108 bytes on the CPU and 96 on the GPU, carrying both
  shadow colours and the light direction even for a flat 1 px grid line, and every flat fill runs
  the full neumorphic fragment shader. `UIRenderer::render` builds new `std::vector`s every frame.
- **No line or curve primitive.** Curves are columns of tiny rectangles: the synth's `FilterCurve`
  draws two 2 px rectangles per 2 px column (about 300 surfaces), `drawCycle` the same, and an
  arrangement audio clip one surface per 2 px of waveform (thousands across a project). They look
  stair-stepped.
- **Scroll views draw all their content.** `ScrollView::draw` draws every child; the scissor hides
  what is out of view, but it is still built, hashed, uploaded and rasterised up to the clip.
- **Text** is printable ASCII from a fixed set of baked sizes, laid out (with a kerning lookup per
  character pair through stb_truetype) every presented frame, and there is no display scale.
  *Not one of the three refactors; noted for later.*

## Stages

Each refactor runs the same four stages, each a wave (or two) of agents, after a contracts step:

| Stage | Meaning | Done when |
|---|---|---|
| Contracts | The orchestrator writes the headers, shader interface and CMake entries the agents build against | `tools/check.sh` passes on every new header |
| **MVP** | The smallest version that delivers the refactor's idea end to end, with tests | builds, every self-test passes, golden images match |
| **Extension** | The rest of the system moved onto it; old paths removed | nothing uses the old path; tests and goldens pass |
| **Performance** | Measured against the baseline, then made faster where the numbers say | the plan's targets met, numbers recorded in the plan |
| **Polish** | Docs, Metrics, cleanup, the orchestrator's visual review of every demo | CLAUDE.md updated, demos checked by eye, committed |

## Before refactor 1: the baseline wave (R0)

Agents can't see the screen, and the first refactor rewrites the shaders the neumorphic look depends
on, so before anything changes we need:

1. **Off-screen rendering**: `engine::Renderer::beginOffscreen(width, height)` and
   `endOffscreen()` (reads the picture back as BGRA bytes), usable with a hidden window.
2. **Golden-image tests**: `--render-selftest` renders a set of scenes (primitives, controls, DAW
   views on the demo project) off screen and compares them with PNGs in `tests/render/`.
   `--render-selftest --update-goldens` rewrites them. Goldens are made from today's renderer
   before any change, and the orchestrator looks at each one before committing it.
3. **Frame statistics**: `ui::FrameStats` (times for update, layout, build and submit; counts of
   primitives, glyphs, images, draw calls and bytes uploaded), read by `Context::stats()`, and
   `VIBIN_FRAME_STATS=1`, which makes the UISystem print one line a second for the main window.
   The baseline numbers for `--demo transport`, `--demo synth`, `--demo pianoroll` and `--demo steps`
   are recorded in each plan before its Performance stage.

| Wave | Agent | Owns (only these files) | Done when |
|---|---|---|---|
| R0.0 | orchestrator | `engine/Renderer.h` (off-screen API), `ui/FrameStats.h`, `ui/Context.h` and `ui/UIRenderer.h` (stats), `tests/RenderSelfTest.h`, `CMakeLists.txt` (test file, `VIBIN_TEST_DIR`) | headers check |
| R0.1 | A off-screen | `src/engine/Renderer.cpp` | `beginOffscreen`/`endOffscreen` as documented |
| R0.1 | B stats | `src/ui/Context.cpp`, `src/ui/UIRenderer.cpp`, `src/ui/UISystem.h/.cpp` | stats filled; `VIBIN_FRAME_STATS` prints |
| R0.1 | C render test | `tests/RenderSelfTest.cpp`, `src/main.cpp` (the flag) | scenes render, compare, update and report |
| R0.2 | orchestrator | `tests/render/*.png` | goldens made, looked at, committed with the baseline numbers |

## Swarm rules (every wave)

- The orchestrator writes the contracts first; they are fixed for the wave. An agent that finds a
  contract wrong stops and reports instead of changing it.
- Each agent owns a disjoint set of files and touches nothing else.
- Agents syntax-check with `tools/check.sh FILE...`. They never run `cmake --build` (one shared
  build tree) and never launch the app.
- Between waves the orchestrator builds (`cmake --build build`, then `tools/link-dev.ps1` while the
  app holds `main.exe`), runs every self-test including `--render-selftest`, reviews the diff, fixes
  or sends back, and commits.
- The look must not change unless a stage says so. A golden that changes needs the orchestrator to
  look at the new image and say why before it is updated.
- The real-time rule is untouched: none of this runs on the audio thread.
