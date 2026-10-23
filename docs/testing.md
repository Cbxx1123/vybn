# Testing

How the self-tests are organised, how to run them, what each tier costs, and the rules for adding to them.
The numbers below were measured on this machine (12 cores, Windows 10, Debug build) before and after the
test rehaul; `tools/test.sh --times` reproduces them.

## Running

```
tools/test.sh                  every suite in parallel, the fast tier          ~15.6 s
tools/test.sh --serial         one suite at a time                              ~29.7 s
tools/test.sh --full           the full-size inputs                             ~51 s
tools/test.sh --bench          also the benchmarks (INFO lines only)
tools/test.sh --times          the slowest checks of each suite
tools/test.sh audio synth      only these suites
```

The VS Code tasks `test` and `test (full)` run the same. A suite is still runnable alone,
`output/vybn.exe --<suite>-selftest`, with the switches below set in the environment. Exit status is 0 only
when every suite passed.

## Architecture

```
tools/test.sh            the runner: one process per suite, longest first, one table, exit code
  output/vybn.exe --X-selftest      a suite (tests/XSelfTest.cpp), run on its own
    tests/Harness.h                 the shared Checker, the tier switches, the Pacer
```

**One process per suite.** The suites share nothing (each builds its own environment, audio system and
temp folders), so they run side by side. The runner starts the longest first, so the wall time is close to
the longest suite's rather than the sum.

**One checker.** `tests/Harness.h` holds the `Checker` every suite used to define for itself (twelve
copies, differing only in column width). A check prints the same `PASS  name  detail` line as before, and
records the time since the previous check, so `VYBN_TEST_TIMES=1` ranks a suite's checks by what they
cost. `finish("suite")` prints one summary line and returns the exit code.

**Three tiers**, chosen by environment variables (`tools/test.sh` sets them from its flags):

| Tier | Switch | What it is for |
|---|---|---|
| fast (default) | none | Every assertion, on inputs just big enough to prove it. While working, and the commit gate. |
| full | `VYBN_TEST_FULL=1` | The same assertions on full-size inputs: the whole 48 s demo song with the real guitar library, 40 clips a track in the idle-frame project. Before a release, or when changing the engine. |
| bench | `VYBN_TEST_BENCH=1` | Also the benchmarks: code that times something and prints it, with nothing to pass or fail. |

A tier changes the *size* of what a check runs on, never whether it asserts: `full() ? big : small` is the
whole use of the switch (`tests::full()`, `tests::bench()` in `Harness.h`).

Two output switches: `VYBN_TEST_TIMES=1` (the slowest checks) and `VYBN_TEST_QUIET=1` (only FAIL, SKIP
and INFO lines and the summary).

**Suites and what each covers** (the long form is in docs/reference/commands.md):

| Suite | Covers | Fast tier |
|---|---|---|
| `audio` | the audio system, tracks, sampler, clips, recording, effects, the deviceless engine | 0.9 s |
| `project` | TOML, the `.vbp` format, history, the store's revisions, autosave, exports' model, the bridge | 0.7 s |
| `input` | signals, the event bus, the keymap, text editing, the file dialog's logic | 0.2 s |
| `notes` | note tools and the step sequencer | 0.1 s |
| `engine` | the scheduler, real-time pool, streaming, the resource manager | 5.3 s |
| `browser` | listings, zip, SFZ, the library | 0.2 s |
| `plugin` | CLAP and VST3 plugins through the sandbox | 2.5 s |
| `synth` | the built-in synthesizer | 4.2 s |
| `export` | mastering, encoders, MIDI, the offline engine, the render queue | 12.1 s |
| `render` | the UI's scenes against golden pictures, the primitive and draw-cache checks | 3.1 s |

**The plugin suite runs alone**, after the others. It asserts deadlines of a few milliseconds (a hung or
crashed plugin must give the 512-frame block back within 0.85 of a block plus 3 ms, 12.1 ms), and CPU load
from the other suites broke those assertions in 2 of 5 parallel runs (12.7 ms and 14.3 ms). The deadline is
the behaviour under test, so the limits were kept and the suite isolated (`TEST_SOLO` overrides the
list). The engine suite, also timing-sensitive, passed under load in every run measured (about 25), so it
stays in the pool.

## Results

Before is the original workflow: each suite run on its own, one after another (the average of two runs).
After is the same, serially, in the fast tier, then in parallel.

| Suite | Before | After (serial) | Where it went |
|---|---:|---:|---|
| export | 55.3 s | 12.1 s | the render checks ran the whole 48 s demo song up to 6 times; a benchmark pass duplicated the stems render |
| engine | 29.8 s | 5.3 s | `sleep_for(2.5 ms)` sleeps a whole timer tick (~15 ms) on Windows, so the "twice real time" Disk-lane checks ran at a seventh of real time |
| project | 15.9 s | 0.7 s | a 300-frame whole-project scan of the *old* change detection, timed for comparison, with nothing asserted |
| render | 14.2 s | 3.1 s | each DAW scene was built three times (picture, cache check, 50-frame timing); now once |
| synth | 4.2 s | 4.2 s | unchanged: the wave bank and 8x7-unison checks are the assertions |
| plugin | 2.6 s | 2.5 s | unchanged: process start-up and deliberate hangs |
| audio, browser, input, notes | 1.4 s | 1.3 s | unchanged |
| **serial total** | **123.3 s** | **29.7 s** | 4.2x |
| **parallel (wall)** | | **15.6 s** | 7.9x; 8 consecutive runs, 15.5 to 15.8 s, no failures |

Full tier: 51 s wall (export on the whole song and the real guitar is 51 s of it). Checks: 903 before, 908
after (five new for track moves and one idle-frame guard, one benchmark pass removed).

## Audit: what was legacy

Every check was reviewed by name and by what it asserts, and the suites by what they cost and why.

**Removed**
- **The old change-detection scan** (`ProjectSelfTest`, idle frames). It re-ran the previous
  implementation (compare the whole project each frame, write the saved text every 15th) on a 160,000-note
  project to print a comparison, `INFO idle frame: ... ms scanning`. It asserted nothing and cost 15.5 s of
  the suite's 16 s. Replaced by a real check, "the revision checks cost nothing per frame on a big project",
  which fails above 5 ms a frame (the old way took ~50 ms and grows with the project).
- **The export benchmark pass** (`The benchmark pass renders`): a seventh full render of the master and
  every stem, which the stems check already does. Now bench-only.
- **The render suite's timed scenes** (`... x50` INFO lines): seven more scene builds that only print.
  Now bench-only.
- **The settled-cache re-renders.** Five DAW scenes were rendered a second time just to run one more frame;
  `renderPicture` already returns that frame beside the picture, so one render serves both the golden and
  the cache check (the picture is taken before the extra frame, so it is unchanged).

**Kept, and confirmed current**
- "An older format is refused" (Project): still the migration policy.
- Every other check names a behaviour the app still has; no suite tests a header only tests include (none
  exists), and no check name repeats.
- `check(true, ...)` appears once (Plugin): the pass branch of an if/else with a `check(false, ...)`.

**Made hermetic**
- The export render fixture used the demo's guitar from the *user's sample library* when installed, so the
  same test rendered different audio on different machines (and spent time decoding it). The fast tier now
  swaps in the built-in synthesizer. The sampler paths are covered with generated samples by the audio and
  engine suites; the full tier still renders the real guitar.
- Still library-dependent: `audio` ("Guitar plays E3", SKIPs when the library is absent). Worth replacing
  with a generated instrument.

**Added**
- `Project::moveTrack` and the "Move track" history label (added with track dragging) had no test. Five
  checks now cover clips and ids travelling with the track, the selection following it or shifting past it,
  bad indexes being refused, and the history label.

## Benchmarks of the runtime systems

Three refactors of the environment layer (`src/env/`) added benchmarks, behind `VYBN_TEST_BENCH`. Run a
suite alone for them (`VYBN_TEST_BENCH=1 ./output/vybn --engine-selftest`), since the other suites' load
moves the numbers (and two engine suites at once clash over their temp folders):

| Line | Suite | What it times |
|---|---|---|
| `INFO pool back to back` | engine | 20,000 batches of 24 one-microsecond jobs on 4 workers, as an offline render calls the pool: us a batch, the share the workers ran, their CPU cycles (`QueryProcessCycleTime` less the calling thread's) |
| `INFO pool paced at 5.3 ms` | engine | 300 batches at a 256-frame block's period, as the live audio thread calls it (with the system timer at 1 ms, else `Pacer` oversleeps a 15.6 ms tick and catches up back to back): median and p99 us, worker cycles a period, batches that came late |
| `INFO mix 24 tracks` | engine | 4 s of 24 tone tracks through the AudioSystem, on one thread and on 4 workers |
| `INFO scheduler wake` | engine | 200,000 `wake(Disk)` calls from one thread (ns each, and how many poller runs they caused), and the wake-to-poll latency of a sleeping lane |
| `INFO dispatch signal` / `bus` / `environment` | input | an emit through 32 hooks; a publish to 8 subscribers, and a post with its dispatch; an `Environment` loop iteration with 16 posted actions |

Measured on this machine (12 cores, the Debug build), the average of three runs of each suite alone, before
and after the refactors:

| Benchmark | Before | After | Change |
|---|---:|---:|---|
| Pool, back to back | 12.0 us a batch | 9.7 us | -19% (workers spin between quick batches; no semaphore calls) |
| Pool, back to back, worker cycles | 135 k a batch | 142 k | +5% (the spinning) |
| Pool, paced at 5.3 ms | 13.8 us median | 13.8 us | none: at the live pace workers sleep at once |
| Pool, paced, worker cycles | 149 k a period | 160 k | +7%, small |
| Mix 24 tracks, 4 workers | 180 ms | 182 ms | within noise (the jobs dwarf the wake) |
| Scheduler `wake` | 348 ns a call | 15 ns | -96% (coalesced: 133,000 poller runs for 200,000 wakes became about 1,100) |
| Wake to poll, median | 6.7 us | 5.0 us | -25% |
| Signal emit, 32 hooks | 2,860 ns | 1,530 ns | -47% (no copy of the hook list) |
| Bus publish, 8 subscribers | 2,700 ns | 700 ns | -74% |
| Bus post and dispatch | 2,990 ns | 870 ns | -71% |
| Environment loop, 16 posts | 4,570 ns | 3,520 ns | -23% |
| Export render speed | 4.2 to 4.7 s/s | 4.4 to 4.7 s/s | within noise |
| Render x50, `VYBN_FRAME_STATS` (`--demo transport`) | | | unchanged (a UI frame neither posts nor emits) |

## Rules for new tests

1. **Use `tests::Checker`** from `Harness.h`; end the suite with `return c.finish("name");`.
2. **Assert, or call it a benchmark.** Code that only times and prints goes behind `bench()`; it is not a
   test and must not cost the default run anything.
3. **Pick the smallest input that proves the property.** If a bigger one adds nothing but time, put it
   behind `full()`. A property that holds on any stretch of a song is proved on 4 s of it.
4. **Don't pace with `sleep_for` under ~15 ms.** Use `tests::Pacer`, which waits to the exact moment.
5. **No dependence on the user's machine** (their sample library, plugins, audio devices). Generate what the
   test needs. A check that can't run says `skip`, with a reason.
6. **Say what a failure means in the check's name**, and put the numbers in the detail.
7. **A new suite** needs its `--x-selftest` flag in `main.cpp`, an entry in `ALL` in `tools/test.sh`
   (longest first), and the file in the `add_executable` list.
8. **Check a new test can fail**: break the code it covers and watch it go red. Two such mutations were
   used on this rehaul (`moveTrack`'s selection shift, and the arrangement's cache key); both were caught.

## Not done, and why

- **Splitting the export suite** into two processes (the render checks and the rest) would bring the wall
  time from ~15.6 s toward ~10 s. It needs a group argument through `runExportSelfTest`; not worth it yet.
- **Coverage.** Nothing measures which source is exercised. A `--coverage` build with gcov would find
  untested code (not stale tests, which the review above looked for); it needs a second build tree.
- **The UI drag** (track reorder, instrument picker order) has no automated check: the render suite draws
  scenes but sends no input. The model-level pieces are tested.
