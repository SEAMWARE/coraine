# Test coverage

Measured **2026-09-02** on `52d683e`, with `make coverage` (see the end for how to
reproduce). Both suites green: 627/627 on mongoc, 577/577 on corDB.

**These are the only coverage figures in the repository.** They used to be repeated
in `testing.md`, `spec-coverage-gaps.md` and the README, and the copies drifted
across two different measurement regimes - for a fortnight the repo stated two
different numbers depending on which page you opened, one of them against a
denominator that covered `coraine/src` alone. Those files now link here.

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
| `make coverage DB=mongoc` | 627 / 627 pass | 83.0% (26188/31534) | 96.0% (1312/1366) | **64.7%** (18133/28030) |
| `make coverage` (corDB) | 577 / 577 pass | 79.3% (23502/29638) | 91.8% (1217/1325) | **62.0%** (16718/26982) |

...and per repository, in the mongoc run, which is where it gets interesting:

| Repository | Lines | Functions | Branches |
|---|---|---|---|
| `corNgsild` | **84.6%** (10247/12116) | 95.2% (589/619) | 67.7% (8709/12869) |
| `coraine/src` | 83.0% (13377/16114) | 97.6% (564/578) | 61.9% (7785/12567) |
| `corJsonld` | 80.0% (825/1031) | 96.5% (55/57) | 67.8% (629/928) |
| `corRest` | **76.5%** (1739/2273) | 92.9% (104/112) | 60.6% (1010/1666) |

The same four in the corDB run:

| Repository | Lines | Functions | Branches |
|---|---|---|---|
| `corNgsild` | 82.6% (10005/12116) | 93.2% (577/619) | 66.5% (8560/12869) |
| `corJsonld` | 79.5% (820/1031) | 96.5% (55/57) | 67.3% (625/928) |
| `coraine/src` | 76.9% (10940/14218) | 89.6% (481/537) | 56.7% (6527/11519) |
| `corRest` | 76.4% (1737/2273) | 92.9% (104/112) | 60.4% (1006/1666) |

`corNgsild` — the library nobody was measuring until 2026-09-01 — is still the
**best-covered** of the four, because the NGSI-LD rules the functests hammer hardest
live there.

The two runs disagree almost entirely in one place. `corRest` and `corJsonld` barely
move between them (76.5% → 76.4%, 80.0% → 79.5%) because nothing in them knows which
database is underneath; the gap is `coraine/src` (83.0% → 76.9%) and, through the 56
mongoc-only tests against corDB's 6, `corNgsild` (84.6% → 82.6%).

## What moves these numbers is usually not new tests

Every headline figure has risen twice this week, and **not one covered line or
function was added either time.**

The second rise, between `e0a5427` and `52d683e`, is the clearest case the file has:

| | before | after |
|---|---|---|
| `corNgsild` functions | 94.4% (589/**624**) | 95.2% (589/**619**) |

The numerator does not move. 589 functions were entered before and 589 after. The
total fell by five, because `ldDatasetIdDedup` and its four helpers were **deleted** —
43 lines and 5 functions that no local write path could reach, since a duplicate
`datasetId` in one payload is rejected by `ldCheckEntity` long before any tiebreaker
could run. Removing them moved the fraction from the denominator side, and the whole
broker figure with it: lines 82.9% → 83.0%, functions 95.7% → **96.0%**, branches
64.6% → 64.7%.

The first rise, a week earlier, was the same shape: `corRest` went 73.6% → 76.5% on
lines and 85.6% → 92.9% on functions without gaining a test of its own, when 253
uncovered lines of unreachable client API were deleted.

⭐ **A rise can be dead code removed rather than behaviour newly tested, and the
headline cannot tell you which.** Read the numerator. If it has not moved, nothing
new is tested — the code got smaller, which is worth doing and is not the same
achievement.

## ⚠️ The figure depends on the environment, and the HA paths are the proof

The HA cache sync (`--high-availability mongo`) rides on a mongo **change stream**,
and a change stream reads the **oplog** — which a standalone mongod does not have.
`corTestParams.sh` probes `isMaster.setName` and decides: on a replica set
`ha_cache_sync.test` is in the run set, on a standalone it is not. It is not reported
as skipped. Nothing in the output says the HA paths went unexercised.

The measurements above were taken against a **single-node replica set**, so they
include the HA code — 108/149 lines, 10/10 functions, 72/114 branches across
`haInit.c`, `haEventApply.c` and `mongocHaWatch.c`.

**CI ran a standalone `mongo:8.0` until 2026-09-02**, and so the nightly's published
figure did not include any of it: those 108 lines and 10 functions read as entirely
unexecuted there, worth −0.34 pp on lines, −0.73 pp on functions and −0.26 pp on
branches against a local run. Since then the jobs that run the suite — `ci.yml`'s
functest matrix and the nightly's coverage and valgrind jobs — use
`quay.io/seamware/mongo-rs`, which is `mongo:8.0` with `--replSet` on the command
line, and initiate the set from `.github/scripts/mongo-rs-init.sh`. The CI figure and
the figure in this file now measure the same thing.

Two jobs deliberately stay on a standalone, and the reason is worth knowing before
anyone "fixes" them: the **perf** job compares against recorded history, and putting
every write through an oplog would move the baseline out from under it — the
comparison would be measuring a change of database topology rather than a change in
the broker. The **ETSI** job has no HA test, so nothing there needs a change stream.

⚠️ Every revision of this file before 2026-09-02 called those lines *untested code
needing an environment the harness does not stand up*. The harness stands it up
perfectly well — it was our own CI that did not, and the sentence read as a property
of the broker. That is the whole reason this section exists: **a coverage figure is a
measurement of an environment as much as of a suite**, and nothing in the output says
which environment produced it.

The corDB run does not enter them either, and correctly so — `ha_cache_sync.test`
declares `REQUIRE_DB: mongoc`, because the sync it tests is a mongo mechanism.

## Why two runs, and why the totals differ

They are separate measurements, not two views of one. A test that pins a
backend-specific answer — mongo's earth model in the fourth digit of a distance, or
the tenant-wide 2dsphere index that makes a GeoProperty and a Property refuse to
share an Attribute name — declares `REQUIRE_DB` and belongs to one run only. Hence
627 tests against mongoc and 577 against corDB.

Each report also excludes the DB plugin that is *not* under test: a corDB run cannot
execute a line of `mongoc.so`, and counting it would measure the choice of backend
rather than the state of the suite.

## Branches is the number that matters

A line is covered if it ran once. `if (a && b)` that only ever runs with both true is
a covered line and two uncovered branches — and the second case is where untested
behaviour hides. That is why branch coverage sits close to twenty points below line
coverage here, and why it is the figure to move.

## "Anything less than 100% is laziness"

It is worth being precise about what the missing 17.0% actually is, because the
reflex answer — *it's all unreachable error handling* — is not what the data says.

Of the **5346 uncovered lines** in the mongoc run — 2737 in `coraine/src`, 1869 in
`corNgsild`, 534 in `corRest`, 206 in `corJsonld`:

| Share | Lines | What it is |
|-------|-------|-----------|
| **60.8%** | 3248 | **ordinary code with no failure guard at all** |
| 25.6% | 1371 | inside a **NULL / invalid-argument guard** |
| **9.1%** | 484 | inside **54 functions the suite never enters at all** |
| 2.4% | 132 | guarded by a **DB / driver failure** — `bson_error_t`, a cursor that fails, `!= DB_OK` |
| 1.2% | 64 | the **NULL-driver-method → 501/422** convention |
| 0.5% | 25 | **defensive** paths — `KT_X`, `default:` on an exhaustive switch, "cannot happen" |
| 0.2% | 12 | `pthread_create` failing, a short `fread`, an allocator returning NULL |
| 0.2% | 10 | **network / socket** failure |

The part that genuinely needs **fault injection** — a database that fails on demand,
a socket that dies mid-write, an allocator that returns NULL — is 154 lines, under
3% of what is uncovered. The largest group by far is ordinary behaviour nobody has
written a test for. Counted in this run: the `success`/`errors` assembly for a batch
operation that half-works (64 lines), `idPattern` handling across subscription
validation, CSR notification and DistOp matching (23), the LRU eviction that runs
when the @context cache is full and the expiry sweep for volatile contexts
(`corLdCache.c`, 14 and 10), `$minDistance` on a geo query (4) and `expiresAt` on a
Context-Source subscription (4).

### The 54 functions that are never entered

This is the one bucket that needs no heuristic — gcov reports an execution count per
function — and the first time it has been measured across all four repositories
rather than `coraine/src` alone.

| Lines | Functions | What they are |
|---|---|---|
| 246 | 25 | **genuinely untested behaviour** |
| 136 | 19 | **shutdown and cleanup** — `troeStop`, `timescaleClose`, `corRestStop`, the three cache `…Release` functions, `ldMqttCleanup`, `onCrash`. They run when the process is going away and assert nothing a test can read |
| 51 | 5 | **parked on purpose** — see below |
| 42 | 1 | `httpEndpointDetect`, startup auto-detection of the broker's own externally-reachable endpoint |
| 9 | 4 | **null-object defaults** — `hookNoop`, `preServiceHookNoop` and two setters nothing calls |

The largest single entries in the untested-behaviour group are `geoEntityValidate`
(23), `attrToNormalized` (19), `stripInfoAttrsFromEntity` and
`stripInfoAttrsFromTemporal` (17 each), `vocabCompactInPlace` (17),
`temporalLatestInstance` (13) and `isNumberString` (13).

The parked group was two families when this file was first written. It is one now:

- **`ringSelfIntersects`** and its four helpers, 51 lines. Deliberately parked, with
  the reason written at the call site: `(void) ringSelfIntersects;` — real fixtures
  have near-coincident vertices that produce mathematically-valid self-intersections,
  and the geo backend resolves interior by the right-hand rule anyway. Kept for a
  strict-validation mode.
- ~~**`ldDatasetIdDedup`** and its four helpers, 43 lines~~ — **deleted.** It was in
  corNgsild's public header and named in its README, and called from nowhere in the
  four repositories. Not merely uncalled but *unreachable*: it implemented the
  § 8.5.3 tiebreaker for a single local write payload, and § 8.5.3 governs versions
  received from registered Context Sources, which `ldDistMergeSourceInto` already
  handles. For one payload the data model is flat — instances are "each identified by
  a unique `datasetId`" — so `ldCheckEntity` answers 400 on all five local write
  paths and nothing reaches a tiebreaker.

⭐ The distinction between those two is the whole value of this bucket. One is code
kept on purpose with the reason recorded at the call site; the other was advertised
library surface no consumer could consume. A list of never-entered functions does not
tell you which is which — reading them does.

The corDB run has 108 never-entered functions and 1498 lines rather than 54 and 484.
The difference is the 56 mongoc-only tests plus the HA test, and it is a property of
the run, not of the code.

**On the method:** the never-entered bucket comes straight from the gcov JSON. The
other buckets are machine-classified — each uncovered line is attributed to the
nearest enclosing guard by indentation, and the guard's text decides the bucket. Two
limits worth knowing before quoting them. The *NULL / invalid-argument* bucket is
coarse: it matches `== NULL`, `!= 0` and `< 0` alike, so it mixes real input
validation with ordinary logic, which makes it an upper bound on "defensive" and the
"ordinary code" figure a lower bound. And line numbers move the moment a file is
edited, so the classification must be recomputed from a coverage run of the *same*
source rather than re-scaled.

⚠️ **The percentages here are not comparable with the hand-sampled ones this file
carried before 2026-09-02.** Those covered `coraine/src` only, against a denominator
that no longer exists, and were bucketed by a different reading of the same idea.

## Reproducing it

```sh
make coverage                # corDB   → coverage-corDB/index.html
make coverage DB=mongoc      # mongoc  → coverage-mongoc/index.html
make coverage-etsi           # ETSI TP suite, instrumenting the libs too
```

Six details, each of which produced a wrong number before it was handled:

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
6. **The mongod must be a replica set** for the run to include the HA paths, and a
   single-node set is enough — `mongod --replSet rs0`, then `rs.initiate()` once. On
   a standalone the numbers are quietly lower and nothing says why; see the section
   above. CI gets this from the `mongo-rs` service image plus
   `.github/scripts/mongo-rs-init.sh`; on a workstation it is a property of the
   mongod you happen to be running.

Afterwards the libs are **left instrumented**, and getting out of that takes
`make libs-rebuild`, not `make libs`: `libs` is each lib's own incremental build, and
a change of compiler flags is invisible to it — the objects are newer than their
sources, so it rebuilds nothing and `corNgsild` stays instrumented inside a build
that calls itself ordinary.

The per-spec-statement view — which statements of TS 104-175 have a test asserting
them — is a different question, tracked in
[`spec-coverage-gaps.md`](spec-coverage-gaps.md).
