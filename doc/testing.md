# Testing

Everything here assumes a build — see [Building from source](building.md).

Functional tests run through `corTest` (installed by the `corLibs` umbrella into
`~/git/corLibs/bin/corTest`):

```sh
make test                    # whole suite (mongoc; use corTest -db corDB for in-memory)
```

Tests live under `test/funcTests/`.

## Coverage

```sh
make coverage                # corDB   → coverage-corDB/index.html
make coverage DB=mongoc      # mongoc  → coverage-mongoc/index.html
make coverage-etsi           # ETSI TP suite → coverage-etsi/index.html
```

Measured **2026-08-27** on `227042c`:

| Run | Tests | Lines | Functions | Branches |
|-----|-------|-------|-----------|----------|
| `DB=mongoc` | 620 / 620 pass | 81.8% | 95.2% | **60.8%** |
| `DB=corDB` | 571 / 571 pass | 76.7% | 89.5% | **56.3%** |

Branch coverage is the honest number of the three, and the one to move. Of the
uncovered lines, some is a failure path no test can reach without fault injection —
a database that fails on demand, a socket that dies mid-write — and **12.3% sits in
35 functions the suite never enters at all**. Nearly half of those, 158 lines, are
the high-availability cache-sync paths, which need a MongoDB **replica set** rather
than a test nobody has written. [`coverage.md`](coverage.md) has the full
breakdown, the method behind it, and why the two runs are separate measurements
rather than two views of one.

The ETSI target instruments the broker **and** the NGSI-LD libs (whole-archived,
so they flush through the broker's gcov runtime) **and** the mongoc/timescale
plugin `.so`s.
