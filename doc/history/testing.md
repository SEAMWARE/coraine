# Testing - history

How the test suite and its rules came about. What the suite is and how to run it is in
[Testing](../testing.md). Newest first; undated entries last.

## 2026-10-02 - the suite from 35 to 14.5 minutes

The whole suite - some 730 tests, a broker (or several) started and stopped for
each - took about **35 minutes** on the machine of `doc/performance.md`. Since
2026-10-02 it takes **14.5** (732 tests, `mongoc`, 868 s). A test of ~2.4 s spent its
time like this:

| | before | after |
|---|---:|---:|
| stopping the broker | ~1.2 s | ~0.15 s with `mongoc` (its own close), ~0.01 s with `corDB` |
| waiting for its port at start-up | up to 0.2 s | up to 0.02 s |
| dropping the database | ~0.3 s (`mongosh`) | ~0.008 s (`corMongoDrop`) |

- **The broker took a second to exit.** On SIGTERM it joins its periodic-work
  thread, which slept for a second at a time; the stop now wakes it (corNgsild
  #56). And the helper polled for the exit with a `pgrep` (~30 ms) every 0.1 s -
  it now watches the pid it started, 10 ms at a time, without forking (#208).
- **The port was polled every 0.2 s** - 20 ms now (corTest#10).
- **Every drop started `mongosh`**, a JavaScript runtime, ~0.3 s - and the suite
  drops 1 466 times. corTools' `corMongoDrop` is one libmongoc connection and a
  command or two: ~8 ms (20 -> 14.5 minutes). `mongosh` stays the fallback where
  `corMongoDrop` was not built (no libmongoc on that machine).

## The functests over cor:// found four server bugs

Running the suite with `COR_TRANSPORT=cor` is how four cor:// server bugs were found
(corRest#22).

## Why functional tests never read the log

A `dds.so` with its traces compiled out (a perf build) timed out every DDS test while
the broker was working fine: the tests waited on trace lines that build never wrote.
Since then a test waits on, and asserts on, what the API says.

## The first real ROS 2 publisher

The first run of `bridge_dds_ros2_roundtrip`, against a publisher that is not ours,
showed that the Enabler hands over an envelope rather than the sample.

## Coverage figures in one place

The coverage figures used to be repeated in `testing.md` as well as `coverage.md`, and
the two copies drifted across two different measurement regimes — `testing.md` went on
quoting a denominator that covered `coraine/src` alone for a fortnight after the
measurement widened to include the three libraries, so the repository stated two
different coverage figures depending on which page you opened. The figures now live in
`coverage.md` only.
