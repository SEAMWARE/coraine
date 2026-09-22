# Testing

Everything here assumes a build — see [Building from source](building.md).

Functional tests run through `corTest` (installed by the `corLibs` umbrella into
`~/git/corLibs/bin/corTest`):

```sh
make test                    # whole suite (mongoc; use corTest -db corDB for in-memory)
```

Tests live under `test/funcTests/`.

## Tests that need something this machine may not have

A few tests cannot run everywhere, and none of them is a choice anyone should
have to remember on a command line — `corTest` DETECTS what is present and those
tests either apply or do not. They leave the run list rather than being reported
as skipped: nobody decided to pass on them.

| Needs | Detected as | Where it is not |
|---|---|---|
| a mongo **replica set** (the HA cache sync rides a change stream) | `-ha yes` | a standalone mongod |
| the **DDS bridge** and a **real ROS 2 publisher** | `-dds yes` | a GitHub runner |

### The DDS tests

`bridge_dds_ros2_roundtrip` is the one test with a real DDS system on the other
side of the bridge, and it stays local on purpose. It needs two things:

```sh
make -C ~/git/corDdsBridge COR_BRIDGE_DDS=ON install   # needs the eProsima stack
docker pull eprosima/vulcanexus:jazzy-desktop          # 6.5 GiB, never pulled by a test
```

The ROS 2 demo talker in that image publishes `std_msgs/String` on `/chatter`,
which is DDS topic `rt/chatter`, and the demo listener is how a sample the
broker PUBLISHES is observed. Everything else about the bridge is tested through
the loopback, which cannot reach this: a DDS participant's topics are compiled
into it, so nobody subscribes to a topic the broker invents and two instances of
our own plugin cannot bootstrap each other. A publisher that is not ours is the
only way — and the first one to run showed that the Enabler hands over an
envelope rather than the sample.

⚠ The containers are started with `--ipc=host` and **not** `--net=host`. Fast DDS
reaches a participant on the same machine over shared memory, so without the
first the broker discovers nothing at all and nothing is logged anywhere; with
both, discovery gets confused and the topics never surface either.


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
