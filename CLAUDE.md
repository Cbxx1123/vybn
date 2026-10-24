# CLAUDE.md

Guidance for Claude Code working in this repository. This page is the short version; the detail is
in `docs/` (see "Where to read more" below). Read the page for the module you are changing before
changing it.

## What this is

VYBN, a DAW in C++17 for Windows: a neumorphic UI drawn with Vulkan (GLFW windows), an audio engine
on miniaudio (WASAPI), CLAP and VST3 plugins hosted in sandboxed processes, and its own project format
(`.vbp`, TOML). Instrument and audio tracks, return and group tracks, a tempo map with time
signature changes, loop recording into take lanes with comping, markers, MIDI import and file drops,
automation, sidechains, audio warping (clips that follow the tempo, warp markers, four stretch
modes, transposition), a piano roll, a step sequencer, a built-in synth and sampler, a library
browser with downloads, undo for every edit, and exports to WAV, AIFF, FLAC, MP3 and MIDI.

## Layout

```
CMakeLists.txt        the project: options, then cmake/ and the modules
cmake/                Dependencies (fetched libraries), Shaders, Targets (helpers), Windows (the icon)
src/
  main.cpp            reads the flags, builds the environment, runs the app or a self-test
  env/                daemons, the scheduler and its thread pools, events, resources
  engine/             windows, input, Vulkan rendering, file dialogs, HTTP, the shell
  ui/                 layout, components, drawing, draw caches, the UI system
  plugin/             plugin hosting: instances, slots, the manager, the sandbox; host/ is the host program (CLAP, VST3)
  audio/              the audio system, devices, instruments, effects, automation, routing, encoders
  daw/
    model/            the project and its rules: Project, ProjectStore, History, Routing, ...
    io/               files and formats: ProjectFile, Toml, Json, Autosave, MIDI export
    library/          the instrument library, downloads, archives, the device catalogue
    audio/            the project played, recorded and rendered: AudioBridge, Recording, RenderJob
    views/            every panel, editor and dialog
    widgets/          shared controls and text formats
    app/              DawApp (the window and its menus), actions, settings, splash, the look
  win/                the icon patcher and the executable's resource
tests/                the self-test suites, plugins/ (the test CLAP and VST3 plugins), render/ (golden images)
tools/                test.sh (runs the suites), coverage.sh (gcov), check.sh (syntax check), link-dev.ps1
shaders/              GLSL, compiled to output/shaders
assets/               the font and the icon
docs/                 architecture/, reference/, design/, testing.md
```

Each module is a static library (`vybn_env`, `vybn_engine`, `vybn_ui`, `vybn_plugin`, `vybn_audio`,
`vybn_daw`, and `vybn_selftests` in tests/) built from the `CMakeLists.txt` in its folder; the program
`vybn` links the DAW and the self-tests. Everything built goes to `output/`: `vybn.exe`,
`vybn-plugin-host.exe`, `vybn-icon-patch.exe`, `shaders/`, `plugins/vybn-test-plugins.clap`, `plugins/vybn-test-plugins.vst3/`.

## Build and run

- Configure once (64-bit TDM-GCC, MinGW Makefiles; never `C:/MinGW`, which is 32-bit and can't link the Vulkan SDK):
  `cmake -S . -B build -G "MinGW Makefiles" -DCMAKE_C_COMPILER=C:/TDM-GCC-64/bin/x86_64-w64-mingw32-gcc.exe -DCMAKE_CXX_COMPILER=C:/TDM-GCC-64/bin/x86_64-w64-mingw32-g++.exe -DCMAKE_BUILD_TYPE=Debug`
- Build: `cmake --build build -j 8`. Clean: `cmake -E rm -rf build output`.
- Run: `output/vybn.exe` (a `.vbp` path opens it; `--silent`, `--size WxH`, `--plugin-folder PATH`,
  `--demo NAME` for the screenshot demos; all in `docs/reference/commands.md`).
- The build is Debug (`-g`), but the real-time audio sources are compiled `-O3` in every build type
  (`VYBN_OPTIMISE_AUDIO`, ON; turn it off to step through them).
- While the app is open it holds `output/vybn.exe`, so a build can't link it: link
  `output/vybn-dev.exe` instead with `tools/link-dev.ps1`, and test that with `VYBN_TEST_EXE`.
- Syntax-check one file without building: `bash tools/check.sh src/daw/model/Project.cpp`.
- Third-party code (GLFW, stb, miniaudio, CLAP, the VST3 interface headers) is fetched at pinned versions on the first configure
  (`cmake/Dependencies.cmake`); the Vulkan SDK comes from `VULKAN_SDK`.

## Test

- `bash tools/test.sh` runs every self-test suite in parallel and prints a table (about 20 s);
  `bash tools/test.sh render` one suite; `--full` the full-size inputs; `--bench` adds the benchmarks;
  `--times` the slowest checks. Each suite is also `output/vybn.exe --<suite>-selftest`
  (audio, project, input, notes, engine, browser, plugin, synth, export, render).
- `bash tools/coverage.sh` prints the lines of `src/` the suites run, from a gcov build of its own
  (`build-coverage/`, `output-coverage/`; about 50 s once built).
- The render suite compares pictures with `tests/render/<scene>.png`. Write a golden with
  `output/vybn.exe --render-selftest --scene NAME --update-goldens` only after looking at the picture.
- Developer switches: `VYBN_FRAME_STATS=1` (a FRAME line a second), `VYBN_VERIFY_CACHE=1` (draw caches
  checked against a fresh paint), `VYBN_VERIFY_STORE=1` (edits the store's revisions missed). Run a
  demo with the verifiers after touching drawing or the model; stderr should hold only FRAME lines.
- Full details, tiers and the rules for new tests: `docs/testing.md`; what each suite covers:
  `docs/architecture/tests.md` and `docs/reference/commands.md`.

## Rules

- Line endings are LF everywhere (`.gitattributes`); keep them so (bash needs it for tools/).
- Includes are rooted at `src/` with the full path (`"daw/model/Project.h"`, `"audio/Track.h"`), also
  inside a module. Tests include their own headers by name (`"Harness.h"`).
- A new `.cpp` goes in its module's `CMakeLists.txt`; a new shader in `cmake/Shaders.cmake`.
- Warnings are `-Wall -Wextra`; a change must add none.
- The audio thread never locks, allocates or frees: buffers are sized in `prepare`, changes arrive
  through atomics and lock-free queues.
- `Track`'s fields are listed by hand in the history (`shellOf`, `sameTrackFields`, ...) and
  `ProjectStore::restore`; a `static_assert` on `sizeof(Track)` in `History.cpp` stops the build when
  one is added, until those are updated; `Clip` has one too (its lists are in `History.cpp`,
  `ProjectSession.cpp`, `AudioBridge::hashClips` and the file). Project and device fields have the same lists
  (`docs/architecture/daw.md`).
- Saved identifiers don't change with the code: the `.vbp` format, its keys, device ids such as
  `vibin.synth` (from before the rename to VYBN).
- Match the code around you: its comment density, naming and plain voice.
- `build/` and `output/` are generated; don't edit them.

## Where to read more

- `docs/architecture/`: `overview.md`, then one page per module (`env`, `engine`, `ui`, `plugin`,
  `audio`, `daw`) and `tests`. The reference for every class and its contracts.
- `docs/reference/`: `commands.md` (every flag, demo and self-test in detail) and `build.md` (the
  build and the traps on this machine).
- `docs/testing.md`: the test harness, tiers, measured times, and the rules for new tests.
- `docs/design/`: the design notes of the larger features and refactors (undo, automation, routing,
  stable ids, the step sequencer, the tempo map, takes and comping, markers and MIDI import, the
  rendering refactors), each with its status and measurements.
