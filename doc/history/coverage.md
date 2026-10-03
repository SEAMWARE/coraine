# Test coverage - history

How the coverage figures and the way they are measured came about: what the figure used to leave
out, the rises that were not new tests, the functions deleted from the never-entered bucket. The
current figures are in [Test coverage](../coverage.md). Newest first.

## Up to 2026-09-03 - the deleted functions

The parked group of never-entered functions was two families when `coverage.md` was first written:
`ringSelfIntersects` (still parked, with the reason at its call site) and:

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

### What the 2026-09-03 pass deleted, and why every one of them was a duplicate

Nine more functions left on the same reasoning, across three repositories. Not one
was a missing wiring — each duplicated a mechanism that was already live, so
*hooking it up* was never the alternative to deleting it:

| Deleted | The live mechanism |
|---|---|
| `ldToNormalized` + `attrToNormalized` | normalized *is* the storage form, and a forward strips `format` from the query string, so an upstream answer arrives normalized too. `ldToConcise` / `ldToSimplified` are the live pair |
| `ldParamExpandV` | `ldExpandParams.c::expandArray` |
| `ldQueryParamValue` | the `ldUrlParams` dispatch table |
| `ldPickOmitNested` | `ldPickOmit` — the deleted one was a byte-identical alias "so call sites can document intent", and no call site ever did |
| `ldBatchErrorListSingleStatus`, `…FirstAsProblemDetails` | `ldBatchErrorsSingleStatus` / `ldBatchErrorAsProblemDetails`, used by all six batch routines |
| `ldSnapshotStatusToString` | status travels as a string; both snapshot exec files carry their own `pickStatus` / `statusFromString` |
| `ldDistOpSend` | `ldDistOpSendReceive` |
| `corLdCacheDownloadingCheck` | `corLdCacheDownloadingAdd`, an atomic test-and-set that also distinguishes a cycle from another thread |
| `timescaleEntityEvent`, `timescaleAttrEvent` | `timescaleEventList`. **Unreachable by construction**: `troeDispatch` checks the list hook first, and timescale registers it |

⚠️ Two of their header comments described behaviour the code did not have.
`LdBatchErrors.h` said "the decision matrix uses this to collapse a uniform-error 207
into a single-status 4xx"; the only user of that type documents the opposite policy
and always answers 207. **A never-called function's doc comment is unverified
prose** — read the caller, not the comment.

## 2026-09-02/03 - mining the never-entered functions

The never-entered bucket was 54 functions and 484 lines on 2026-09-02, with 25 functions in the
untested-behaviour group; on 2026-09-03 it was 31 and 268, with three. Mining this bucket is what
produced the 2026-09-03 tests. `geoEntityValidate` and `attrInstanceOf` were listed as gaps in
`coverage.md` for a day, and both were chased as gaps before anyone followed the caller.

## Up to 2026-09-03 - three rises of the headline in a week, two without a new test

The headline has now risen three times in a week. **Twice, not one covered line or
function was added.**

The clearest case the file has, between `e0a5427` and `52d683e`:

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

### The third rise, and what it looks like when the numerator *does* move

`52d683e` → `577e633` is the counter-example, and the reason the rule is "read the
numerator" rather than "distrust every rise":

| | numerator | denominator |
|---|---|---|
| lines | 26188 → 26347 (**+159 newly covered**) | 31534 → 31448 (−86) |
| functions | 1312 → 1324 (**+12 newly entered**) | 1366 → 1355 (−11) |
| branches | 18133 → 18318 (**+185 newly taken**) | 28030 → 27984 (−46) |

Both effects at once — eight new tests, and three repositories' worth of unreachable
code deleted — but this time the numerator carries most of it. The twelve functions
are exactly the ones the new tests were written for; the eleven that left the
denominator are the ones nothing could reach.

Which is also why the two halves have to be reported separately. Lines alone would
say "+0.8 pp" and leave a reader to guess whether the suite grew or the code shrank.
It was both, and only the numerator says so.

## Until 2026-09-02 - CI on a standalone mongod

**CI ran a standalone `mongo:8.0` until 2026-09-02**, and so the nightly's published
figure did not include any of the HA code: those 108 lines and 10 functions read as entirely
unexecuted there, worth −0.34 pp on lines, −0.73 pp on functions and −0.26 pp on
branches against a local run. Since then the jobs that run the suite use
`quay.io/seamware/mongo-rs`.


⚠️ Every revision of `coverage.md` before 2026-09-02 called those lines *untested code
needing an environment the harness does not stand up*. The harness stands it up
perfectly well — it was our own CI that did not, and the sentence read as a property
of the broker. That is the whole reason the environment section of `coverage.md` exists.

## Until 2026-09-01 - half the broker outside the figure

`corRest`, `corNgsild` and `corJsonld` are static archives whole-archived into the broker. Until
2026-09-01 `make coverage` linked them exactly as `make libs` had left them: ordinary flags, no gcov
counters. They were therefore not reported as *uncovered*, they were **absent** — in neither numerator
nor denominator — and `--root coraine/src` kept the omission out of sight. Roughly half the broker's C
sat outside a number presented as the broker's. `corNgsild` was the library nobody was measuring.

## Before 2026-09-02 - figures in several places

The coverage figures used to be repeated in `testing.md`, `spec-coverage-gaps.md` and the README,
and the copies drifted across two different measurement regimes - for a fortnight the repo stated two
different numbers depending on which page you opened, one of them against a denominator that covered
`coraine/src` alone. Those files now link to `doc/coverage.md`.

⚠️ **The percentages in `coverage.md` are not comparable with the hand-sampled ones it
carried before 2026-09-02.** Those covered `coraine/src` only, against a denominator
that no longer exists, and were bucketed by a different reading of the same idea.

## A stale coverage tree

After `corRamDB` became `corDB`, a stale coverage tree (`.gcno` files of the renamed sources) added
~700 phantom uncovered lines and moved the figure from 80.7% to 77.5%. `make coverage` rebuilds the
tree from scratch.
