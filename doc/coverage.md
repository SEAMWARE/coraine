# Test coverage

Measured **2026-09-01** on `bfa4d85` plus the new collation test below, with
`make coverage` (see the end for how to reproduce). Both suites green.

## The figure covers the broker, and the broker is four repositories

`corRest`, `corNgsild` and `corJsonld` are static archives whole-archived into the
binary — `corNgsild` alone is 12k lines, most of the NGSI-LD rulebook, three quarters
the size of `coraine/src`. Until 2026-09-01 `make coverage` linked them exactly as
`make libs` had left them: ordinary flags, no gcov counters. They were therefore not
reported as *uncovered*, they were **absent** — in neither numerator nor denominator —
and `--root coraine/src` kept the omission out of sight. Roughly half the broker's C
sat outside a number presented as the broker's.

| Run | Tests | Lines | Functions | Branches |
|-----|-------|-------|-----------|----------|
| `make coverage DB=mongoc` | 623 / 623 pass | 82.7% (26130/31613) | 95.1% (1310/1378) | **64.3%** (18080/28102) |
| `make coverage` (corDB) | 573 / 573 pass | 78.9% (23451/29717) | 90.9% (1215/1337) | **61.6%** (16675/27054) |

...and per repository, in the mongoc run, which is where it gets interesting:

| Repository | Lines | Functions | Branches |
|---|---|---|---|
| `corNgsild` | **84.3%** (10240/12153) | 94.4% (589/624) | 67.4% (8705/12915) |
| `coraine/src` | 82.9% (13356/16107) | 97.6% (565/579) | 61.7% (7765/12581) |
| `corJsonld` | 80.0% (825/1031) | 96.5% (55/57) | 67.8% (629/928) |
| `corRest` | **73.6%** (1709/2322) | 85.6% (101/118) | 58.5% (981/1678) |

The widening was expected to hurt, and it did not. `corNgsild` — the library nobody
was measuring — is the **best-covered** of the four, and branch coverage went *up*,
60.8% → 64.3%, because the NGSI-LD rules the functests hammer hardest live there.
`corRest` is the one thin spot: the HTTP layer's error and negotiation paths, and the
only place in the four where fewer than nine functions in ten are entered at all.

The two runs disagree almost entirely in one place. `corRest` and `corJsonld` barely
move between them (73.6% → 73.5%, 80.0% → 79.5%) because nothing in them knows which
database is underneath; the gap is `coraine/src` (82.9% → 76.9%) and, through the 56
mongoc-only tests against corDB's 6, `corNgsild` (84.3% → 82.3%).

⚠️ **No figure recorded before 2026-09-01 is comparable with these.** The denominator
roughly doubles (16216 lines → 31613). A drop against an older number in this file is
the measurement widening, not the suite regressing.

⚠️ And every pre-2026-09-01 figure measured a build nobody ships. Both coverage
targets passed their flags in `DFLAGS`, which **replaces** each lib's own definition
rather than adding to it — so `corNgsild` lost `-DANSI` *and* `-DCOR_WITH_ICU` and
compiled its dependency-free collation approximation while the broker went on linking
libicu. The instrumentation now goes through an `EXTRA_CFLAGS` hook, appended last.
The suite could not tell the two builds apart until
`orderby_collation_locale.test` was written to do exactly that.

## Why two runs, and why the totals differ

They are separate measurements, not two views of one. A test that pins a
backend-specific answer — mongo's earth model in the fourth digit of a distance, or
the tenant-wide 2dsphere index that makes a GeoProperty and a Property refuse to
share an Attribute name — declares `REQUIRE_DB` and belongs to one run only. Hence
623 tests against mongoc and 573 against corDB.

Each report also excludes the DB plugin that is *not* under test: a corDB run cannot
execute a line of `mongoc.so`, and counting it would measure the choice of backend
rather than the state of the suite.

## Branches is the number that matters

A line is covered if it ran once. `if (a && b)` that only ever runs with both true is
a covered line and two uncovered branches — and the second case is where untested
behaviour hides. That is why branch coverage sits close to twenty points below line
coverage here, and why it is the figure to move.

## "Anything less than 100% is laziness"

It is worth being precise about what the missing 17.3% actually is, because the
reflex answer — *it's all unreachable error handling* — is not what the data says.

⚠ **The breakdown that follows is `coraine/src` only, from 2026-08-27.** It was
hand-sampled against a gcov run of that source, and line numbers move the moment a
file is edited, so it cannot simply be re-scaled. Of today's **5483 uncovered lines**,
2751 are in `coraine/src` and **2732 are in the three libs and have never been
classified at all** — that is the next piece of this analysis to do, and `corRest`
at 73.6% is where to start.

Of the **2951 uncovered lines** in the 2026-08-27 mongoc run, one bucket was measured
directly and was the one that moved:

| Share | What it is |
|-------|-----------|
| **12.3%** (362) | inside **35 functions the suite never enters at all** — and 158 of those lines, 9 functions, are the HA cache-sync paths |

That bucket was 18.3% (586 lines, 38 functions) on 2026-08-20. It shrank because
most of what was in it turned out to be **dead code rather than untested code** —
see the note at the top. What is left in it divides cleanly:

- **The HA cache-sync paths** — `mongocHaWatch.c`, `haEventApply.c`, and the tenant
  and @context cache refresh/drop hooks they drive. 158 lines. These need a MongoDB
  **replica set**, because change streams do not exist without one. That is an
  environment the functest harness does not stand up, not a test nobody wrote.
- **`httpEndpointDetect`** (41 lines) — startup auto-detection of the broker's own
  externally-reachable endpoint.
- **Genuinely untested behaviour**, and the honest remainder: `mergeAttrsNonOverriding`
  (inclusive-mode merge from a Context Source), `geoEntityValidate`,
  `stripInfoAttrsFromTemporal` / `stripInfoAttrsFromEntity`.
- **Shutdown paths** — `timescalePoolCloseAll`, `timescaleClose`, `troeStop` — which
  run when the process is going away and assert nothing a test could read.

⚠ **The other buckets below have not been recomputed** since 2026-08-20, and their
denominator has changed (3196 uncovered lines then, 2951 now), so the percentages
would be wrong if carried across unchanged. They are kept as the shape of the
answer, not as current figures, and the classification is worth redoing:

| Share (of 3196, 2026-08-20) | What it is |
|-------|-----------|
| 10.9% (349) | guarded by a **DB / driver failure** — `bson_error_t`, a cursor that fails, `!= DB_OK` |
| 4.8% (155) | the **NULL-driver-method → 501/422** convention |
| 2.2% (71) | **defensive** paths — `KT_X`, `default:` on an exhaustive switch, "cannot happen" |
| 1.3% (42) | **network / socket** failure |
| 0.5% (18) | `pthread_create` failing, a short `fread`, an allocator returning NULL |
| 61.8% (1975) | **ordinary code with no failure guard at all** |

The part that genuinely needs **fault injection** — a database that fails on demand,
a socket that dies mid-write, an allocator that returns NULL — was about one line in
six at that measurement. The largest remaining group is **partial-success assembly**:
the code that builds the `success` / `errors` arrays when a batch operation
half-works, plus optional request shapes — `$minDistance` on a geo query, `idPattern`
in a snapshot, `expiresAt` on a Context-Source subscription.

⚠ The earlier estimate of a **~83% ceiling** without mocks also predates the
deletions, and the denominator it was computed against no longer exists. Today's
81.8% is much closer to that old ceiling than the arithmetic of new tests would
suggest — because the ceiling itself moved when the unreachable code went.

**On the method:** the classification comes from the gcov JSON — every uncovered line
is attributed to the function it sits in, and, when that function does run, to the
nearest enclosing guard. It is a heuristic; the buckets were sampled by hand and the
`ordinary code` bucket in particular was read line by line before being described
that way. The line numbers must come from a coverage run of the *same* source, since
editing a file shifts every line under it.

## Reproducing it

```sh
make coverage                # corDB   → coverage-corDB/index.html
make coverage DB=mongoc      # mongoc  → coverage-mongoc/index.html
make coverage-etsi           # ETSI TP suite, instrumenting the libs too
```

Five details the target handles, each of which produced a wrong number before it did:

1. **The coverage tree is rebuilt from scratch.** `.gcno` files of a renamed or
   deleted source are never cleaned up, and gcovr reports those vanished files as
   entirely unexecuted — after `corRamDB` became `corDB`, a stale tree added ~700
   phantom uncovered lines and moved the figure from 80.7% to 77.5%.
2. **`SEAMWARE_PLUGIN_DIR` is exported alongside `COR_PLUGIN_DIR`.** The first is how
   the harness spells a plugin path; the second is how the *broker* resolves a short
   name a test passes itself (`coraineStart --apiPlugins admin`). Without it the run
   loads the installed, uninstrumented `admin.so` and the whole admin plugin reads 0%
   in a run whose tests exercise it.
3. **gcovr is told not to abort on GCC bug 68080.** gcov occasionally reports a
   negative hit count in a threaded binary; without
   `--gcov-ignore-parse-errors=negative_hits.warn_once_per_file` a twenty-minute run
   ends with no report at all. The flag needs **gcovr >= 8.3**, pinned in
   `corLibs/docker/Dockerfile.ci-nightly`: on 8.2 it was read only by the text
   `.gcov` parser, and on >= 8.3 without it gcovr does not fail — it silently drops
   the whole affected file from the report and exits 0.
4. **The libs are instrumented and the root moves up to the sibling directory.**
   Otherwise `corRest`, `corNgsild` and `corJsonld` link in with no counters and
   vanish from both sides of the fraction, as they did until 2026-09-01.
5. **The flags go in `EXTRA_CFLAGS`, never `DFLAGS`.** `DFLAGS` is a plain variable
   in each lib's makefile, so setting it on the command line *replaces* the lib's own
   defaults — and a `DFLAGS +=` inside the makefile is then ignored too, because `+=`
   never appends to a command-line variable. `corNgsild` lost `-DANSI` and
   `-DCOR_WITH_ICU` that way and compiled its non-ICU collation fallback while the
   broker went on linking libicu. `EXTRA_CFLAGS` is appended last, so `-O0` beats
   `-O2` and `-Wno-error` beats `-Werror` without displacing anything.

Afterwards the libs are **left instrumented**, and getting out of that takes
`make libs-rebuild`, not `make libs`: `libs` is each lib's own incremental build, and
a change of compiler flags is invisible to it — the objects are newer than their
sources, so it rebuilds nothing and `corNgsild` stays instrumented inside a build
that calls itself ordinary.

The per-spec-statement view — which statements of TS 104-175 have a test asserting
them — is a different question, tracked in
[`spec-coverage-gaps.md`](spec-coverage-gaps.md).
