# Test coverage

Measured **2026-09-03** on `577e633`, with `make coverage` (see the end for how to
reproduce). Both suites green: 634/634 on mongoc, 584/584 on corDB.

**These are the only coverage figures in the repository.** `testing.md`,
`spec-coverage-gaps.md` and the README link here. How it came about:
[history](history/coverage.md).

## The figure covers the broker, and the broker is four repositories

`corRest`, `corNgsild` and `corJsonld` are static archives whole-archived into the
binary — `corNgsild` alone is 12k lines, most of the NGSI-LD rulebook, three quarters
the size of `coraine/src`. `make coverage` instruments them as well, so the figure is
the broker's: all four repositories.

| Run | Tests | Lines | Functions | Branches |
|-----|-------|-------|-----------|----------|
| `make coverage DB=mongoc` | 634 / 634 pass | 83.8% (26347/31448) | 97.7% (1324/1355) | **65.5%** (18318/27984) |
| `make coverage` (corDB) | 584 / 584 pass | 80.1% (23664/29552) | 93.5% (1229/1314) | **62.8%** (16908/26936) |

...and per repository, in the mongoc run, which is where it gets interesting:

| Repository | Lines | Functions | Branches |
|---|---|---|---|
| `corNgsild` | **85.8%** (10336/12046) | 97.9% (597/610) | 68.7% (8811/12817) |
| `coraine/src` | 83.5% (13447/16107) | 98.4% (568/577) | 62.6% (7868/12577) |
| `corJsonld` | 80.7% (825/1022) | 98.2% (55/56) | 68.1% (629/924) |
| `corRest` | **76.5%** (1739/2273) | 92.9% (104/112) | 60.6% (1010/1666) |

The same four in the corDB run:

| Repository | Lines | Functions | Branches |
|---|---|---|---|
| `corNgsild` | 83.8% (10097/12046) | 95.9% (585/610) | 67.6% (8665/12817) |
| `corJsonld` | 80.2% (820/1022) | 98.2% (55/56) | 67.6% (625/924) |
| `coraine/src` | 77.5% (11010/14211) | 90.5% (485/536) | 57.3% (6610/11529) |
| `corRest` | 76.4% (1737/2273) | 92.9% (104/112) | 60.5% (1008/1666) |

`corNgsild` is the **best-covered** of the four, because the NGSI-LD rules the
functests hammer hardest live there.

The two runs disagree almost entirely in one place. `corRest` and `corJsonld` barely
move between them (76.5% → 76.4%, 80.7% → 80.2%) because nothing in them knows which
database is underneath; the gap is `coraine/src` (83.5% → 77.5%) and, through the 56
mongoc-only tests against corDB's 6, `corNgsild` (85.8% → 83.8%).

## What moves these numbers is usually not new tests

⭐ **A rise can be dead code removed rather than behaviour newly tested, and the
headline cannot tell you which.** Read the numerator. If it has not moved, nothing
new is tested — the code got smaller, which is worth doing and is not the same
achievement. Three rises of the headline, two of them without a new test, are in the
[history](history/coverage.md).

## ⚠️ The figure depends on the environment, and the HA paths are the proof

The HA cache sync (`--high-availability mongo`) rides on a mongo **change stream**,
and a change stream reads the **oplog** — which a standalone mongod does not have.
`corTestParams.sh` probes `isMaster.setName` and decides: on a replica set
`ha_cache_sync.test` is in the run set, on a standalone it is not. It is not reported
as skipped. Nothing in the output says the HA paths went unexercised.

The measurements above were taken against a **single-node replica set**, so they
include the HA code — 108/149 lines, 10/10 functions, 72/114 branches across
`haInit.c`, `haEventApply.c` and `mongocHaWatch.c`.

Against a standalone those 108 lines and 10 functions read as entirely unexecuted:
−0.34 pp on lines, −0.73 pp on functions and −0.26 pp on branches. The jobs that run
the suite — `ci.yml`'s functest matrix and the nightly's coverage and valgrind jobs —
use `quay.io/seamware/mongo-rs`, which is `mongo:8.0` with `--replSet` on the command
line, and initiate the set from `.github/scripts/mongo-rs-init.sh`. The CI figure and
the figure in this file measure the same thing.

Two jobs deliberately stay on a standalone, and the reason is worth knowing before
anyone "fixes" them: the **perf** job compares against recorded history, and putting
every write through an oplog would move the baseline out from under it — the
comparison would be measuring a change of database topology rather than a change in
the broker. The **ETSI** job has no HA test, so nothing there needs a change stream.

⚠️ **A coverage figure is a measurement of an environment as much as of a suite**,
and nothing in the output says which environment produced it.

The corDB run does not enter them either, and correctly so — `ha_cache_sync.test`
declares `REQUIRE_DB: mongoc`, because the sync it tests is a mongo mechanism.

## Why two runs, and why the totals differ

They are separate measurements, not two views of one. A test that pins a
backend-specific answer — mongo's earth model in the fourth digit of a distance, or
the tenant-wide 2dsphere index that makes a GeoProperty and a Property refuse to
share an Attribute name — declares `REQUIRE_DB` and belongs to one run only. Hence
634 tests against mongoc and 584 against corDB.

Each report also excludes the DB plugin that is *not* under test: a corDB run cannot
execute a line of `mongoc.so`, and counting it would measure the choice of backend
rather than the state of the suite.

## Branches is the number that matters

A line is covered if it ran once. `if (a && b)` that only ever runs with both true is
a covered line and two uncovered branches — and the second case is where untested
behaviour hides. That is why branch coverage sits close to twenty points below line
coverage here, and why it is the figure to move.

## "Anything less than 100% is laziness"

It is worth being precise about what the missing 16.2% actually is, because the
reflex answer — *it's all unreachable error handling* — is not what the data says.

Of the **5101 uncovered lines** in the mongoc run — 2660 in `coraine/src`, 1710 in
`corNgsild`, 534 in `corRest`, 197 in `corJsonld`:

| Share | Lines | What it is |
|-------|-------|-----------|
| **63.0%** | 3216 | **ordinary code with no failure guard at all** |
| 26.9% | 1374 | inside a **NULL / invalid-argument guard** |
| 5.3% | 268 | inside **31 functions the suite never enters at all** |
| 2.6% | 132 | guarded by a **DB / driver failure** — `bson_error_t`, a cursor that fails, `!= DB_OK` |
| 1.3% | 64 | the **NULL-driver-method → 501/422** convention |
| 0.5% | 25 | **defensive** paths — `COR_X`, `default:` on an exhaustive switch, "cannot happen" |
| 0.2% | 12 | `pthread_create` failing, a short `fread`, an allocator returning NULL |
| 0.2% | 10 | **network / socket** failure |

The part that genuinely needs **fault injection** — a database that fails on demand,
a socket that dies mid-write, an allocator that returns NULL — is 154 lines, about
3% of what is uncovered. The largest group by far is ordinary behaviour nobody has
written a test for. Counted in this run: the `success`/`errors` assembly for a batch
operation that half-works (64 lines), `idPattern` handling across subscription
validation, CSR notification and DistOp matching (23), the LRU eviction that runs
when the @context cache is full and the expiry sweep for volatile contexts
(`corLdCache.c`, 14 and 10), `$minDistance` on a geo query (4) and `expiresAt` on a
Context-Source subscription (4).

With the never-entered bucket nearly exhausted, this is where the remaining work is —
not in finding a function nobody calls. The four
files holding the most of it are, in the mongoc run:

| File | Uncovered | Covered |
|---|---|---|
| `corRest/corRestClientMulti.c` | 187 | 58.9% |
| `mongoc/mongocEntityQuery.c` | 172 | 73.9% |
| `corRest/corRestClient.c` | 122 | 69.0% |
| `corNgsild/ldEntityMatch.c` | 120 | 64.6% |

The last two rows of that list are the pushdown and the in-broker evaluator of the
same predicates: `mongocEntityQuery.c` builds the pushdown and
`ldEntityMatch.c` evaluates in the broker, and every predicate that appears in both
has two implementations that must agree. The multi-request client is the other half —
the pooled-connection retry after a peer has gone away, and the response-buffer
growth past 8 KB that the single-request client exercises exactly once.

### The 31 functions that are never entered

This is the one bucket that needs no heuristic — gcov reports an execution count per
function.

| Lines | Functions | What they are |
|---|---|---|
| 125 | 18 | **shutdown and cleanup** — `troeStop`, `timescaleClose`, `corRestStop`, the three cache `…Release` functions, `ldMqttCleanup`, `onCrash`. They run when the process is going away and assert nothing a test can read |
| 51 | 5 | **parked on purpose** — see below |
| 42 | 1 | `httpEndpointDetect`, startup auto-detection of the broker's own externally-reachable endpoint |
| 41 | 3 | **genuinely untested behaviour** — the whole remainder, named below |
| 9 | 4 | **null-object defaults** — `hookNoop`, `preServiceHookNoop` and two setters nothing calls |

Of the **three** in the untested-behaviour group, only ONE is a gap in the suite:

- **`corRestClientResponseHeader`** (8) is the redirect `Location` lookup. Reaching it
  needs a Context Source that answers 3xx **with a `Location` header**, and corTestClient's
  `/mock/reply` can set a status but not headers. A real gap, and the only one here.

- **`geoEntityValidate`** (23) and **`attrInstanceOf`** (10) are **not gaps at all** —
  they are an artefact of what a single run measures. `geoEntityValidate` lives in
  `plugins/shared/`, which no run excludes, but it is called only from
  `plugins/currentState/corDB/`, which a mongoc run *does* exclude. It can therefore
  never be non-zero in the mongoc column, whatever anyone writes. All four of its
  diagnostics are already asserted, by `geoproperty_degenerate_polygon` and three
  others. `attrInstanceOf` is the same shape one level up: it is reached only
  when the broker evaluates `q` in-process, which a mongoc query does not do.

  ⭐ **The mongoc run's never-entered list contains functions only the corDB path can
  reach, and vice versa.** The corDB figure below already says this about itself — "a
  property of the run, not of the code" — and the mirror case is easy to miss, because
  a mongoc-only zero looks exactly like an untested function until you follow the
  caller.

The bucket is close to exhausted: what remains is either deliberate, or shutdown code
that outlives the assertions a test could make.

The parked group is one family:

- **`ringSelfIntersects`** and its four helpers, 51 lines. Deliberately parked, with
  the reason written at the call site: `(void) ringSelfIntersects;` — real fixtures
  have near-coincident vertices that produce mathematically-valid self-intersections,
  and the geo backend resolves interior by the right-hand rule anyway. Kept for a
  strict-validation mode.

The corDB run has 85 never-entered functions and 1282 lines rather than 31 and 268.
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

## Reproducing it

```sh
make coverage                # corDB   → coverage-corDB/index.html
make coverage DB=mongoc      # mongoc  → coverage-mongoc/index.html
make coverage-etsi           # ETSI TP suite, instrumenting the libs too
```

Six details, each of which gives a wrong number when it is missed:

1. **The coverage tree is rebuilt from scratch.** `.gcno` files of a renamed or
   deleted source are never cleaned up, and gcovr reports those vanished files as
   entirely unexecuted.
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
   vanish from both sides of the fraction.
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
