# Test coverage

Measured **2026-08-20** on `e8e6bb6`, with `make coverage` (see below for how to
reproduce). Both suites green.

| Run | Tests | Lines | Functions | Branches |
|-----|-------|-------|-----------|----------|
| `make coverage DB=mongoc` | 613 / 613 pass | 80.7% (13374/16570) | 93.7% (563/601) | **60.8%** (7806/12843) |
| `make coverage` (corDB) | 563 / 563 pass | 74.4% (10922/14674) | 85.7% (480/560) | **55.4%** (6529/11795) |

## Why two runs, and why the totals differ

They are separate measurements, not two views of one. A test that pins a
backend-specific answer — mongo's earth model in the fourth digit of a distance, or
the tenant-wide 2dsphere index that makes a GeoProperty and a Property refuse to
share an Attribute name — declares `REQUIRE_DB` and belongs to one run only. Hence
613 tests against mongoc and 563 against corDB.

Each report also excludes the DB plugin that is *not* under test: a corDB run cannot
execute a line of `mongoc.so`, and counting it would measure the choice of backend
rather than the state of the suite.

## Branches is the number that matters

A line is covered if it ran once. `if (a && b)` that only ever runs with both true is
a covered line and two uncovered branches — and the second case is where untested
behaviour hides. That is why branch coverage sits ~20 points below line coverage
here, and why it is the figure to move.

## "Anything less than 100% is laziness"

It is worth being precise about what the missing 19.3% actually is, because the
reflex answer — *it's all unreachable error handling* — is not what the data says.

Of the **3196 uncovered lines** in the mongoc run:

| Share | What it is |
|-------|-----------|
| **18.3%** (586) | inside **38 functions the suite never enters at all** |
| **10.9%** (349) | guarded by a **DB / driver failure** — `bson_error_t`, a cursor that fails, `!= DB_OK` |
| **4.8%** (155) | the **NULL-driver-method → 501/422** convention |
| **2.2%** (71) | **defensive** paths — `KT_X`, `default:` on an exhaustive switch, "cannot happen" |
| **1.3%** (42) | **network / socket** failure |
| **0.5%** (18) | `pthread_create` failing, a short `fread`, an allocator returning NULL |
| **61.8%** (1975) | **ordinary code with no failure guard at all** |

So the part that genuinely needs **fault injection** — a database that fails on
demand, a socket that dies mid-write, an allocator that returns NULL — is about
**one line in six**. Everything else is reachable by a test that nobody has written
yet, and the two biggest groups say exactly what those tests would be:

- **Distributed operations.** The never-entered functions are dominated by forwarding:
  `forwardQueryToCSR`, `forwardBatchToCSR` (in all four batch operations),
  `forwardTemporalQueryToCSR`, `forwardEntityMapToCSR`, `forwardDeleteAttr`,
  `shapeUpstreamBody`, `mergeAttrsNonOverriding`. The suite does run distributed
  tests, but not through these paths.
- **Partial-success assembly.** Much of the 61.8% is the code that builds the
  `success` / `errors` arrays when a batch operation half-works, plus optional
  request shapes — `$minDistance` on a geo query, `idPattern` in a snapshot,
  `expiresAt` on a Context-Source subscription.

A realistic ceiling without mocks is therefore somewhere near **83%** of lines, and
the honest reading of today's 80.7% is not "the rest is unreachable" but "most of the
rest is untested, and we know which parts".

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
