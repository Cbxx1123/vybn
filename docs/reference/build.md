# Build system notes

How the build is put together, and the traps on this machine.

## The CMake files

- `CMakeLists.txt` (the root): the C++ standard, the default Debug build, the options
  (`VYBN_OPTIMISE_AUDIO`), `VYBN_OUTPUT_DIR` (`output/`), then the files in `cmake/` and the
  modules (`src/`, `tests/`).
- `cmake/Dependencies.cmake`: the third-party code, fetched by `FetchContent` at pinned versions on
  the first configure (network access and `git` needed), offered as targets: `glfw`, and the
  interface targets `vybn_stb`, `vybn_miniaudio` and `vybn_clap`.
- `cmake/Shaders.cmake`: the `shaders` target (`shaders/*.vert|frag` to `output/shaders/*.spv` by
  `glslc` from the SDK's `Bin`). A new shader goes in its `SHADER_SOURCES`.
- `cmake/Targets.cmake`: the helpers the module files use: `vybn_module(name SOURCES ...)` (a static
  library, includes rooted at `src/`, the warnings), `vybn_warnings`, `vybn_standalone` (statically
  linked, for the helper programs) and `vybn_output` (into `output/`, or a folder under it).
- `cmake/Windows.cmake`: `vybn_add_icon(target)`, the executable's icon resource.
- `src/CMakeLists.txt`: the modules in dependency order and the `vybn` program; a `CMakeLists.txt` in
  each of `src/env`, `engine`, `ui`, `plugin` (with the plugin host), `audio`, `daw` and `win` (the icon
  patcher) lists its sources and links what it uses; `tests/CMakeLists.txt` the self-tests library
  and the test plugins.

Modules build on one another in one direction: `env`; `engine` (on env), `ui` (on engine); `plugin`
(on env), `audio` (on plugin); `daw` (on all of them); `vybn_selftests` (on daw). Each links the
Windows libraries its own code needs: engine `dwmapi`, `comdlg32`, `winhttp`, `shell32`; plugin
`userenv` and `advapi32` (the sandbox's AppContainers and access lists); audio `winmm` (MIDI) and
`mfuuid`, `ole32` (MP3 through Media Foundation); daw `gdiplus` (the loading screen), `dwmapi`,
`winmm`, `shell32`.

## Traps and details

- Always pass `-G "MinGW Makefiles"` on Windows. Without it, CMake picks Visual Studio, a multi-config
  generator that writes to `output/Debug/vybn.exe` and breaks the run and launch paths.
- Always pass the TDM-GCC-64 compilers. The default MinGW (`C:/MinGW`, GCC 6.3) is 32-bit, and the
  Vulkan SDK's `vulkan-1.lib` is 64-bit, so linking fails with it.
- The executable's icon is compiled by hand with the `windres` next to the compiler in use
  (`cmake/Windows.cmake`, `-F pe-x86-64`), not as CMake's RC language: the default MinGW's 32-bit
  `windres` is first on `PATH`, and CMake's own detection both picked it (its objects fail to link:
  `i386 architecture ... is incompatible`) and, when told otherwise, looped re-configuring forever.
  If the build tree is ever made again, configure with the TDM-GCC-64 command in CLAUDE.md.
- `VYBN_OPTIMISE_AUDIO` (ON by default) compiles the audio engine's real-time sources (`AudioSystem`,
  `Track`, `DeviceChain`, `Effects`, `Synth`, `Sampler`, the players, mastering and encoders) at -O3 in
  every build type, keeping their debug information: the app runs as a Debug build, and at -O0 that
  code took two to three times as long (GCC 10 only vectorises at -O3). Configure with
  `-DVYBN_OPTIMISE_AUDIO=OFF` to step through it in the debugger. The list is in
  `src/audio/CMakeLists.txt`.
- `Window.cpp` sets `_WIN32_WINNT` to Windows 10 at its very top, before any include, because MinGW's
  standard headers otherwise fix it at XP and hide `SM_CXPADDEDBORDER`.
- The plugin host (`vybn-plugin-host`, from `src/plugin/host/HostMain.cpp`) and the test plugins
  (`vybn-test-plugins`, a `MODULE` named `vybn-test-plugins.clap` in `output/plugins/`) link
  `-static`, so the sandbox needs to read nothing but the executable and system DLLs. The program
  depends on both, and on the icon patcher, so one build makes them all.
- Vulkan comes from the SDK in `VULKAN_SDK` (currently `C:\VulkanSDK\1.3.250.1`), found by
  `find_package(Vulkan)`.
- stb provides the header-only `stb_truetype.h` (its implementation is compiled in
  `src/ui/Font.cpp`), `stb_image.h`'s inflate (compiled in `src/daw/library/Archive.cpp` with only the
  PNG decoder, for zip files) and, in the browser self-test only, `stb_image_write.h`'s deflate.
  It is a `SYSTEM` include, so its warnings stay quiet.
- miniaudio 0.11.25 is only populated (its own CMake project isn't built). Its implementation is
  compiled once, in `src/audio/Miniaudio.cpp`. Its switches are interface definitions of
  `vybn_miniaudio`, so every file that includes it agrees: `MA_NO_ENGINE`, `MA_NO_NODE_GRAPH`,
  `MA_NO_RESOURCE_MANAGER`, and on Windows `MA_ENABLE_ONLY_SPECIFIC_BACKENDS` plus `MA_ENABLE_WASAPI`.
  Only `AudioSystem.cpp`, `SampleSet.cpp`, `AudioFile.cpp` and `Miniaudio.cpp` include `miniaudio.h`.
- The font is `assets/fonts/Inter-Medium.ttf` (Inter 4.0, SIL Open Font License; the licence is next
  to it). The app finds it through `ENGINE_ASSET_DIR` (the source tree's `assets/`) and falls back to
  `C:/Windows/Fonts/segoeui.ttf` if it's missing. The shaders are read from `ENGINE_SHADER_DIR`
  (`output/shaders`). Both are definitions of `vybn_engine`. The self-tests have `VYBN_TEST_DIR` (the
  source tree's `tests/`, for the golden images) and `ENGINE_TEST_PLUGIN` (the test plugins' file).
- The debugger is `C:/TDM-GCC-64/bin/gdb.exe`, set in `.vscode/launch.json`. It must match the 64-bit
  compiler. The VS Code tasks in `.vscode/tasks.json` wrap configure, build, build and run, and clean.
- `tools/check.sh FILE...` syntax-checks files with the build's include paths and defines, without
  touching `build/` or `output/`. Use it while the app is running, since that holds
  `output/vybn.exe`. It defines stand-ins for `ENGINE_SHADER_DIR`, `ENGINE_ASSET_DIR`,
  `ENGINE_TEST_PLUGIN` and `VYBN_TEST_DIR`, so files that read them compile.
- `tools/link-dev.ps1` links the compiled build to `output/vybn-dev.exe` instead of
  `output/vybn.exe`, for testing while the app holds `output/vybn.exe` (run it after the build; the
  autosave recovery check treats `vybn-dev.exe` as the app). `tools/test.sh` tests it with
  `VYBN_TEST_EXE=output/vybn-dev.exe`.
- `tools/coverage.sh` builds the coverage tree (`build-coverage/`, `-DVYBN_COVERAGE=ON
  -DVYBN_OPTIMISE_AUDIO=OFF -DVYBN_OUTPUT_DIR=output-coverage`, with the compiler from
  `build/CMakeCache.txt`), runs the suites against it and prints gcov's line coverage (docs/testing.md).
  `VYBN_OUTPUT_DIR` is a cache variable, so another tree can put its output elsewhere.
- `build/`, `build-*/`, `output/` and `output-*/` are generated (and ignored by git). Don't edit files in them.
