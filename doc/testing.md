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

**The figures live in [`coverage.md`](coverage.md) and only there.** They used to be
repeated here as well, and the two copies drifted across two different measurement
regimes — this file went on quoting a denominator that covered `coraine/src` alone
for a fortnight after the measurement widened to include the three libraries, so the
repository stated two different coverage figures depending on which page you opened.
One number, one place.

Branch coverage is the honest one of the three, and the one to move: a line is
covered if it ran once, so `if (a && b)` that only ever runs with both true is a
covered line and two uncovered branches — and the second case is where untested
behaviour hides.

[`coverage.md`](coverage.md) has the current figures, what the uncovered lines
actually are, the method behind that classification, and why the two DB runs are
separate measurements rather than two views of one.

The ETSI target instruments the broker **and** the NGSI-LD libs (whole-archived,
so they flush through the broker's gcov runtime) **and** the mongoc/timescale
plugin `.so`s.
