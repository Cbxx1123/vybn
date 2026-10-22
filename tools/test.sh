#!/usr/bin/env bash
# Runs the self-test suites (each is `output/main.exe --<suite>-selftest`) and prints one table.
#
#   tools/test.sh                  every suite, in parallel, the fast tier (see docs/testing.md)
#   tools/test.sh audio synth      only these suites
#   tools/test.sh --full           the full-size inputs (the whole demo song, 160,000 notes...): slower
#   tools/test.sh --bench          also run the benchmarks (timings printed as INFO; nothing to pass or fail)
#   tools/test.sh --times          also list each suite's slowest checks
#   tools/test.sh --serial         one suite at a time (also -j 1); -j N runs N at once
#   tools/test.sh --logs DIR       keep each suite's output in DIR (default: a temp folder, kept only on failure)
#   tools/test.sh --list           the suite names
#
# Suites are separate processes, so none shares state with another, and they are started longest first so
# the wall time is close to the longest suite's rather than the sum. The plugin suite asserts deadlines of a
# few milliseconds (a hung or crashed plugin must hand the block back in time), which CPU load from its
# neighbours breaks about one run in three, so it runs alone, after the others. TEST_SOLO names the suites
# that do (a space-separated list; TEST_SOLO= for none).
# Exit status: 0 when every suite passed, 1 when any failed or couldn't run, 2 for a bad command line.

set -u
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# VIBIN_TEST_EXE names another build to test (output/main-dev.exe, linked by tools/link-dev.ps1 while
# the app holds main.exe)
EXE="${VIBIN_TEST_EXE:-$ROOT/output/main.exe}"
[ -x "$EXE" ] || EXE="$ROOT/output/main"

# Longest first (measured; see docs/testing.md)
ALL="export engine synth render plugin audio project browser input notes"
SOLO="${TEST_SOLO-plugin}"

jobs_max=$(nproc 2>/dev/null || echo 4)
[ "$jobs_max" -gt 10 ] && jobs_max=10
times=0; logs=""
want=()
while [ $# -gt 0 ]; do
  case "$1" in
    --full) export VIBIN_TEST_FULL=1 ;;
    --bench) export VIBIN_TEST_BENCH=1 ;;
    --times) times=1 ;;
    --serial) jobs_max=1 ;;
    -j) shift; jobs_max="$1" ;;
    --logs) shift; logs="$1" ;;
    --list) echo $ALL; exit 0 ;;
    -h|--help) sed -n '2,19p' "$0"; exit 0 ;;
    -*) echo "unknown option $1" >&2; exit 2 ;;
    *) want+=("$1") ;;
  esac
  shift
done
suites=${want[*]:-$ALL}
for s in $suites; do
  case " $ALL " in *" $s "*) ;; *) echo "unknown suite $s (try --list)" >&2; exit 2 ;; esac
done
if [ ! -x "$EXE" ]; then echo "build first: $EXE not found" >&2; exit 2; fi

keep=1
if [ -z "$logs" ]; then logs="$(mktemp -d)"; keep=0; fi
mkdir -p "$logs"
[ $times -eq 1 ] && export VIBIN_TEST_TIMES=1

now() { date +%s%N; }

run_suite() { # name
  local s="$1" t0 t1 code
  t0=$(now)
  "$EXE" --"$s"-selftest > "$logs/$s.log" 2>&1
  code=$?
  t1=$(now)
  echo "$code $(( (t1 - t0) / 1000000 ))" > "$logs/$s.result"
}

is_solo() { case " $SOLO " in *" $1 "*) return 0 ;; esac; return 1; }

wall0=$(now)

# The parallel group, longest first, at most $jobs_max at a time
pids=()
for s in $suites; do
  is_solo "$s" && [ "$jobs_max" -gt 1 ] && continue
  while [ ${#pids[@]} -ge "$jobs_max" ]; do wait "${pids[0]}"; pids=("${pids[@]:1}"); done
  run_suite "$s" &
  pids+=($!)
done
for p in "${pids[@]}"; do wait "$p"; done

# The suites that need the machine to themselves
if [ "$jobs_max" -gt 1 ]; then
  for s in $suites; do is_solo "$s" && run_suite "$s"; done
fi

wall=$(( ($(now) - wall0) / 1000000 ))

# The table
printf "\n%-9s %-6s %7s %7s %9s\n" suite status checks skipped ms
total_cpu=0; failed=0
for s in $suites; do
  read -r code ms < "$logs/$s.result" 2>/dev/null || { code=1; ms=0; }
  pass=$(grep -c '^PASS' "$logs/$s.log"); fail=$(grep -c '^FAIL' "$logs/$s.log"); skip=$(grep -c '^SKIP' "$logs/$s.log")
  status=ok
  if [ "$code" -ne 0 ] || [ "$fail" -ne 0 ]; then status=FAIL; failed=1; fi
  printf "%-9s %-6s %7s %7s %9s\n" "$s" "$status" "$pass" "$skip" "$ms"
  total_cpu=$(( total_cpu + ms ))
done
printf "%-9s %-6s %7s %7s %9s\n" "wall" "" "" "" "$wall"
w=$(( wall > 0 ? wall : 1 ))
printf "(%s ms of suite time in %s ms: %s.%sx)\n" "$total_cpu" "$wall" $(( total_cpu / w )) $(( (total_cpu * 10 / w) % 10 ))

if [ $times -eq 1 ]; then
  echo; echo "Slowest checks:"
  for s in $suites; do grep '^TIME' "$logs/$s.log" | sed "s|^TIME *||; s|^|$s  |"; done | sort -t' ' -k3 -n -r | sort -k2 -n -r | head -15
fi

if [ $failed -ne 0 ]; then
  echo; echo "Failures:"
  grep -H '^FAIL' "$logs"/*.log | sed "s|$logs/||; s|\.log:|: |" | head -40
  echo "(full output in $logs)"
  exit 1
fi
[ $keep -eq 0 ] && rm -rf "$logs"
exit 0
