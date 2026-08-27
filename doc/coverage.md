# Test coverage

Measured **2026-08-27** on `227042c`, with `make coverage` (see below for how to
reproduce). Both suites green.

| Run | Tests | Lines | Functions | Branches |
|-----|-------|-------|-----------|----------|
| `make coverage DB=mongoc` | 620 / 620 pass | 81.8% (13265/16216) | 95.2% (556/584) | **60.8%** (7721/12691) |
| `make coverage` (corDB) | 571 / 571 pass | 76.7% (10980/14320) | 89.5% (486/543) | **56.3%** (6551/11643) |

> **What moved, and why it was not new tests.** The previous measurement
> (2026-08-20, `e8e6bb6`) read 80.7% / 93.7% / 60.8% on mongoc. Four new functest
> cases account for a little of the rise; almost all of it is **twenty dead static
> functions being deleted**. They were unreachable — nothing called them — so they
> could never be covered, and every one of them was dragging the denominator down.
> The mongoc function count fell from 604 to 584, which is exactly the twenty. A
> coverage figure depressed by unreachable code understates the suite rather than
> flattering it.

## Why two runs, and why the totals differ

They are separate measurements, not two views of one. A test that pins a
backend-specific answer — mongo's earth model in the fourth digit of a distance, or
the tenant-wide 2dsphere index that makes a GeoProperty and a Property refuse to
share an Attribute name — declares `REQUIRE_DB` and belongs to one run only. Hence
620 tests against mongoc and 571 against corDB.

Each report also excludes the DB plugin that is *not* under test: a corDB run cannot
execute a line of `mongoc.so`, and counting it would measure the choice of backend
rather than the state of the suite.

## Branches is the number that matters

A line is covered if it ran once. `if (a && b)` that only ever runs with both true is
a covered line and two uncovered branches — and the second case is where untested
behaviour hides. That is why branch coverage sits ~20 points below line coverage
here, and why it is the figure to move.

## "Anything less than 100% is laziness"

It is worth being precise about what the missing 18.2% actually is, because the
reflex answer — *it's all unreachable error handling* — is not what the data says.

Of the **2951 uncovered lines** in the mongoc run, one bucket is measured directly
and is the one that moved:

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

Three details the target handles, each of which produced a wrong number before it did:

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
   ends with no report at all.

The per-spec-statement view — which statements of TS 104-175 have a test asserting
them — is a different question, tracked in
[`spec-coverage-gaps.md`](spec-coverage-gaps.md).
