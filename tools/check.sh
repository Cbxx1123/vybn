#!/bin/sh
# Syntax-checks C++ files with the build's flags, without touching build/ or output/ (safe to run
# from several agents at once). Usage: sh tools/check.sh src/daw/StepSequence.cpp [more files...]
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
status=0
for f in "$@"; do
    /c/TDM-GCC-64/bin/g++ -std=c++17 -Wall -Wextra -fsyntax-only \
        -I"$ROOT/src" -I"$ROOT/tests" -I"$ROOT/build/_deps/glfw-src/include" \
        -isystem "$ROOT/build/_deps/stb-src" -isystem "$ROOT/build/_deps/miniaudio-src" \
        -isystem "$ROOT/build/_deps/clap-src/include" -isystem "C:/VulkanSDK/1.3.250.1/Include" \
        -DMA_ENABLE_ONLY_SPECIFIC_BACKENDS -DMA_ENABLE_WASAPI -DMA_NO_ENGINE -DMA_NO_NODE_GRAPH -DMA_NO_RESOURCE_MANAGER \
        -DENGINE_SHADER_DIR='"output/shaders"' -DENGINE_ASSET_DIR='"assets"' -DENGINE_TEST_PLUGIN='"plugins"' -DVYBN_TEST_DIR='"tests"' \
        "$f" && echo "OK   $f" || { echo "FAIL $f"; status=1; }
done
exit $status
