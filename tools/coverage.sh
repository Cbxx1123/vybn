#!/usr/bin/env bash
# Which lines of the program's modules (src/) the self-tests run, from gcov. A build of its own:
# build-coverage/, its output in output-coverage/ (VYBN_COVERAGE on, the audio code not optimised so
# its lines are its own), so the everyday build is left alone.
#
#   tools/coverage.sh               configure (once) and build it, run every suite, print the table
#   tools/coverage.sh audio synth   only these suites (the counters start again at each run)
#   tools/coverage.sh --report      the table again, from the last run
#   tools/coverage.sh --all         the table lists every file (by default the 40 least covered)
#
# The table is each source file's lines run, least covered first, and the total. Only the modules are
# counted: not main.cpp, the tests or the plugin host (whose process is sandboxed and can't write the
# counters). The suites run one at a time, since two processes ending together would write the same
# counter files. A file no suite reaches has no counters and shows as 0%.

set -u
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD="$ROOT/build-coverage"
OUT="$ROOT/output-coverage"

report=0; all=0
suites=()
while [ $# -gt 0 ]; do
  case "$1" in
    --report) report=1 ;;
    --all) all=1 ;;
    -h|--help) sed -n '2,15p' "$0"; exit 0 ;;
    -*) echo "unknown option $1" >&2; exit 2 ;;
    *) suites+=("$1") ;;
  esac
  shift
done

# The compiler the everyday build uses (64-bit TDM-GCC), and its gcov beside it
cxx=$(sed -n 's/^CMAKE_CXX_COMPILER:[A-Z]*=//p' "$ROOT/build/CMakeCache.txt" 2>/dev/null)
[ -n "$cxx" ] || cxx="C:/TDM-GCC-64/bin/x86_64-w64-mingw32-g++.exe"
cc="${cxx%g++.exe}gcc.exe"
gcov="$(dirname "$cxx")/gcov.exe"
[ -x "$gcov" ] || gcov=gcov

if [ $report -eq 0 ]; then
  if [ ! -f "$BUILD/CMakeCache.txt" ]; then
    cmake -S "$ROOT" -B "$BUILD" -G "MinGW Makefiles" -DCMAKE_C_COMPILER="$cc" -DCMAKE_CXX_COMPILER="$cxx" \
      -DCMAKE_BUILD_TYPE=Debug -DVYBN_COVERAGE=ON -DVYBN_OPTIMISE_AUDIO=OFF -DVYBN_OUTPUT_DIR="$OUT" || exit 1
  fi
  cmake --build "$BUILD" -j 8 || exit 1
  find "$BUILD/src" -name '*.gcda' -delete
  VYBN_TEST_EXE="$OUT/vybn.exe" bash "$ROOT/tools/test.sh" --serial "${suites[@]}"
  echo
fi

# The sources as gcov names them (C:/..., not /c/...)
SRC=$(cygpath -m "$ROOT/src" 2>/dev/null || echo "$ROOT/src")

# Each module source's counters: gcov reads its .gcno and .gcda (beside its object) and says how many of
# its lines ran. A header is left out: it is counted in every file that includes it, each partly.
rows=$(
  find "$BUILD/src" -name '*.cpp.gcno' | while read -r note; do
    dir=$(dirname "$note")
    "$gcov" -n -o "$dir" "$note" 2>/dev/null | awk -v root="$SRC/" '
      /^File / { file = substr($0, 7, length($0) - 7); gsub(/\\/, "/", file) }
      /^Lines executed:/ && file ~ /\.cpp$/ {
        split($2, a, ":"); pct = a[2] + 0; n = $4 + 0
        i = index(file, root); if (i == 0) { i = index(tolower(file), tolower(root)) }
        if (i > 0) { printf "%d %d %s\n", int(pct * n / 100 + 0.5), n, substr(file, i + length(root)) }
        file = ""
      }'
  done | sort -u -k3,3
)
if [ -z "$rows" ]; then
  echo "no counters: run tools/coverage.sh first" >&2
  exit 1
fi

echo "$rows" | awk -v all=$all '
  { run[NR] = $1; lines[NR] = $2; name[NR] = $3; total += $2; ran += $1 }
  END {
    for (i = 1; i <= NR; i++) pct[i] = lines[i] ? 100 * run[i] / lines[i] : 100
    # least covered first (a simple sort: a few hundred files)
    for (i = 1; i <= NR; i++) order[i] = i
    for (i = 2; i <= NR; i++) { k = order[i]; j = i - 1; while (j > 0 && pct[order[j]] > pct[k]) { order[j + 1] = order[j]; j-- } order[j + 1] = k }
    printf "%7s %7s  %s\n", "lines", "run", "file"
    shown = all ? NR : (NR < 40 ? NR : 40)
    for (i = 1; i <= shown; i++) { k = order[i]; printf "%7d %6.1f%%  %s\n", lines[k], pct[k], name[k] }
    if (shown < NR) printf "%7s %7s  (%d more; --all lists them)\n", "", "", NR - shown
    printf "%7d %6.1f%%  total, %d files\n", total, total ? 100 * ran / total : 0, NR
  }'
