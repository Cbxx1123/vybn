# VYBN

A digital audio workstation for Windows, written in C++17.

- **Tracks**: instrument tracks (the built-in synth, the sample library, or CLAP plugins) and audio
  tracks (imported or recorded), with return tracks fed by sends and group tracks that fold.
- **Editing**: an arrangement with automation lanes and a tempo map (tempo changes and ramps, time
  signature changes), a piano roll with note tools and an arpeggiator, a step sequencer, an audio
  clip editor, and undo for every edit.
- **Mixing**: a mixer with sends, groups and a master volume, built-in effects (EQ, filter, gate and
  compressor with sidechains, limiter, saturation, delay, reverb), CLAP effects in a sandbox, and delay
  compensation across the whole routing.
- **Export**: WAV, AIFF, FLAC, MP3 and MIDI, normalised to a peak or to EBU R128 loudness, rendered
  in the background.

The interface is drawn with Vulkan; audio runs on miniaudio (WASAPI). Each CLAP plugin runs in a
sandboxed process of its own.

## Building

Needs Windows, CMake 3.16+, the 64-bit TDM-GCC toolchain, the Vulkan SDK (`VULKAN_SDK` set) and git
(the first configure fetches GLFW, stb, miniaudio and the CLAP headers).

```
cmake -S . -B build -G "MinGW Makefiles" -DCMAKE_C_COMPILER=C:/TDM-GCC-64/bin/x86_64-w64-mingw32-gcc.exe -DCMAKE_CXX_COMPILER=C:/TDM-GCC-64/bin/x86_64-w64-mingw32-g++.exe
cmake --build build -j 8
output/vybn.exe
```

## Testing

```
bash tools/test.sh
```

runs every self-test suite (audio, project, input, notes, engine, browser, plugin, synth, export and
the render suite, which compares the views with golden images) and prints a table.

## Layout

- `src/`: the program, by module: `env` (daemons and threads), `engine` (windows, input, Vulkan),
  `ui` (the UI layer), `plugin` (CLAP hosting), `audio` (the audio engine) and `daw` (the DAW: model,
  files, library, the audio bridge, views and the app).
- `tests/`: the self-tests, the test plugins and the golden images.
- `docs/`: the architecture of each module, the command and build reference, the testing guide and
  the design notes of the larger features.
- `cmake/`, `tools/`, `shaders/`, `assets/`: the build's pieces, scripts, the shaders, the font and icon.

`CLAUDE.md` is the short guide for working in the code; `docs/architecture/` has the detail.
