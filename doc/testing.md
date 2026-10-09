# Testing

Everything here assumes a build — see [Building from source](building.md).

Functional tests run through `corTest` (installed by the `corLibs` umbrella into
`~/git/corLibs/bin/corTest`):

```sh
make test                    # whole suite (mongoc; use corTest -db corDB for in-memory)
```

Tests live under `test/funcTests/`.

How it came about: [history](history/testing.md).

## How long the suite takes

The whole suite - 732 tests, a broker (or several) started and stopped for each -
takes **14.5 minutes** with `mongoc` (868 s, 2026-10-02, on the machine of
`doc/performance.md`). The fixed cost of a test:

| | |
|---|---:|
| stopping the broker | ~0.15 s with `mongoc` (its own close), ~0.01 s with `corDB` |
| waiting for its port at start-up | up to 0.02 s |
| dropping the database | ~0.008 s (`corMongoDrop`) |

- **Stopping.** On SIGTERM the broker wakes its periodic-work thread and joins it
  (corNgsild #56); the helper watches the pid it started, 10 ms at a time,
  without forking (#208).
- **The port** is polled every 20 ms (corTest#10).
- **The drop** is corTools' `corMongoDrop`: one libmongoc connection and a
  command or two. `mongosh` is the fallback where `corMongoDrop` is not built (no
  libmongoc on that machine) - a JavaScript runtime, ~0.3 s a drop, and the suite
  drops 1 466 times.

Sending a cor:// request instead of an HTTP one saves nothing measurable - a
request is a process start either way (`COR_TRANSPORT=cor`, next).

## The functests over cor://

```sh
COR_TRANSPORT=cor ~/git/corLibs/bin/corTest -db mongoc
```

runs every test it can on the broker's binary API instead of HTTP: each broker
also listens on cor:// (its HTTP port + 1000), and `corCurl` sends the request
with `corRequest --curl` (corTools), which prints exactly what `corCurl` prints
for an HTTP request - so every expectation holds unchanged. What only HTTP can
carry stays on curl: a body that is not JSON, a text answer, HEAD, a POST without
a body (411), a method the broker does not have, a body over 1 MiB (413).
`COR_TRANSPORT_TRACE=<file>` logs which transport each request took. The whole
suite passes that way.


## corDB in the functests: persistent, as MongoDB

With `-db corDB` every broker has `--dbDir` (corDB's persistence): a directory per role under
`COR_DB_PERSIST_DIR` (default `/tmp/corTest-dbDir`). As with MongoDB, a (re)start keeps what is there
and `corDbDrop` empties it - the role's directory, or with `-tenant` that tenant's - so a test that
stops and starts its broker finds its data on either database, and every write of the suite goes
through corDB's log. A test that gives its broker a `--dbDir` of its own keeps it.


## Functional tests never read the log

A test waits on, and asserts on, what the **API** says — never on a trace line in
the broker's log. A trace is there only when the trace levels include it *and* the
code that writes it was built with its traces, so a test waiting on one times out
against a build without them (a perf build) while the broker works fine. So no test
sets `COR_TRACE_LEVELS`, and the suite passes with `-traceLevels ""`.

What a test waits on instead — the helpers are in `test/funcTests/corTestFunctions.sh`:

| Waiting for | Read from | Helper |
|---|---|---|
| a DDS endpoint to be discovered | the Channel's `endpointDiscovered` | `ddsServiceAwait`, `bridgeChannelAwait` |
| a counter — publishes, goals, cancels, requests | the Channel's counters ([§3.3b](bridge-channels-details.md#33b-what-crossed--discovery-and-counters)) | `bridgeChannelGet`, `bridgeChannelAtLeast`, `bridgeChannelsCount` |
| a sample the Bridge dropped | the Bridge's `samplesDropped` | `bridgeGet`, `bridgeAtLeast` |
| a goal to end | `GET /channels/{id}/goals/{goalId}`: 200 while in progress, 404 after | `bridgeGoalInProgress`, `bridgeGoalEndedAwait` |
| a member a bridge writes (`status`, `feedback`, `reply`, …) | the entity | `attrMemberAwait` |
| how a goal went, once its instance is gone | the attribute's temporal history (`--troe timescale --troeSync`) | `bridgeGoalHistory`, `bridgeGoalHistoryAwait` |

A **warning** (`COR_W`) is different: no trace level and no build switches it off, and
a test may still check one was given.

To prove a test does not lean on a trace, run it with `-traceLevels ""` against a
`dds.so` built with `COR_T` compiled out (`nm -D /opt/seamware/plugins/bridge/dds.so |
grep corLogTraceLevels` then prints nothing).

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
only way, and it reaches what the loopback cannot: the Enabler hands over an
envelope rather than the sample.

⚠ The containers are started with `--ipc=host` and **not** `--net=host`. Fast DDS
reaches a participant on the same machine over shared memory, so without the
first the broker discovers nothing at all and nothing is logged anywhere; with
both, discovery gets confused and the topics never surface either.


## Coverage

```sh
make coverage                # mongoc  → coverage-mongoc/index.html
make coverage DB=corDB       # corDB   → coverage-corDB/index.html
make coverage-etsi           # ETSI TP suite → coverage-etsi/index.html
```

**The figures live in [`coverage.md`](coverage.md) and only there.** One number,
one place.

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
