# Extreme performance

The published image is built to be fast for everybody: one build, tuned on a general mix of requests.
A deployment that knows what it does - mostly notifying writes, say, or mostly queries, or a burst of
creates - can have a broker built for exactly that: **`make tune`** trains the compiler on a
description of the workload, measures the knobs that depend on it, and builds the result, as a binary
or as an image.

This page: what you get without it, what it changes, how to describe your workload, how to run it, and
what it is worth.

## What you get out of the box

The published image (and `make pgo`):

| | out of the box | what decides it |
|---|---|---|
| compiler optimisation | `-O2`, **profile-guided** (PGO) | trained on [`test/perf/pgoTrain.sh`](https://github.com/SEAMWARE/coraine/blob/main/test/perf/pgoTrain.sh): perfRun's request shapes on corDB (in RAM, and on disk with history), a write that notifies, the three-broker forwarding chain over HTTP and cor:// - every kind of work, none of it in your proportions |
| memory allocator | glibc's `malloc` | the C library |
| corDB's store lock | `--dbLockPrefer reads` - readers first when readers and writers both wait | glibc's rwlock default |
| HTTP server | libmicrohttpd, `--connectionPoolSize` the cores (corDB in RAM) or 32 | [building details](building-details.md) |
| corDB on disk | `--dbSync interval` (every 100 ms), a snapshot every 64 MiB of log | [corDB's README](https://github.com/SEAMWARE/corDB) |

`make tune` changes the first three - the ones that are a property of the WORKLOAD, not of the
deployment. The last two are the deployment's (how durable a write must be, how many cores there are)
and stay options you set.

## The three knobs

- **The profile.** PGO lays the code out for the paths the training ran - the hot ones together, the
  branches the right way round. Trained on your workload, it is laid out for your paths. Against the
  same source without PGO the general profile is worth +1-10 % per core
  ([performance](performance.md), "Profile-guided").
- **The allocator.** glibc's `malloc`, jemalloc or tcmalloc, loaded with `LD_PRELOAD` - no rebuild. On
  concurrent creates into a durable corDB both are faster than glibc's (+11 % and +18 %); which is
  fastest is the workload's.
- **Whom corDB's store lock lets in first** (`--dbLockPrefer reads|writes`). Writers first: creates
  +5 %, batch creates +15 %, merges +7.5 %; reads and notifying `PATCH`es -3-6 %
  ([performance](performance.md), "Writes that notify, and which writer the lock lets in first").
  mongoc has no such lock - for it, the option is not there and `make tune` measures the allocators
  only.

## Building it yourself

Everything starts from a source build - [Building from source](building.md): the Cor-Libs as siblings
(`corLibs/bootstrap.sh` clones and builds them), then the broker. On top of that, `make tune` needs:

- `wrk` (the load) and `python3` (the workload generator), as `make pgo` does;
- `corTestClient`, from `corLibs/bin` - it receives the notifications and counts them;
- jemalloc and tcmalloc, to measure them: `apt-get install libjemalloc2 libtcmalloc-minimal4`
  (a missing one is left out of the comparison).

```sh
cd coraine
make tune WORKLOAD=my-workload.json        # ~15 min: the instrumented build, the training, the comparison
make install_pgo                           # the tuned build -> /usr/local/bin, /opt/seamware
```

It ends with one line, and writes **`tune-result.json`**:

```
tune: best --dbLockPrefer writes LD_PRELOAD=/usr/lib/x86_64-linux-gnu/libtcmalloc_minimal.so.4 - 173721 requests/s, +11.1 % against the out-of-the-box settings - tune-result.json
```

Then run the broker with what it says:

```sh
LD_PRELOAD=/usr/lib/x86_64-linux-gnu/libtcmalloc_minimal.so.4 coraine -fg --database corDB --dbDir /var/lib/coraine --dbLockPrefer writes ...
```

⚠️ **Measure on a quiet machine.** The comparison runs the workload on this machine; a browser, a
build or a video call running beside it is measured too (`make tune` warns when a browser is up). Run
it where the broker will run, or on the same kind of machine - which allocator wins can depend on the
CPU.

## The workload file

JSON. Every member has a default; an example is
[`test/perf/tune/workload.example.json`](https://github.com/SEAMWARE/coraine/blob/main/test/perf/tune/workload.example.json).

```json
{
  "store": { "database": "corDB", "persistent": true, "history": "none" },

  "entities": {
    "count": 10000,
    "types": [
      { "type": "Vehicle", "share": 0.8,
        "attributes": [
          { "name": "speed",    "type": "Property", "value": 42, "observedAt": true, "subAttributes": 1 },
          { "name": "location", "type": "GeoProperty" },
          { "name": "isParked", "type": "Relationship" },
          { "name": "brand",    "type": "Property", "value": "Mercedes" }
        ] },
      { "type": "Parking", "share": 0.2,
        "attributes": [
          { "name": "free",     "type": "Property", "value": 10 },
          { "name": "name",     "type": "LanguageProperty" },
          { "name": "location", "type": "GeoProperty" }
        ] }
    ]
  },

  "subscriptions": [
    { "count": 100, "select": "perEntity", "watchedAttributes": ["speed"] },
    { "count": 5,   "select": "byType", "type": "Parking", "q": "free<3" }
  ],

  "load": {
    "concurrency": 50,
    "mix": { "patch": 0.80, "query": 0.10, "retrieve": 0.05, "create": 0.05 },
    "patch": { "attributes": ["speed"] },
    "query": { "type": "Vehicle", "q": "speed>50", "limit": 20 }
  },

  "duration": "30s"
}
```

### `store` - where the entities live

| member | values | default |
|---|---|---|
| `database` | `corDB`, `mongoc` | `corDB` |
| `persistent` | corDB on disk (`--dbDir`, a fresh directory per run) | `false`: in RAM |
| `history` | `none`, `corDB` (corDB's own), `timescale` | `none` |

### `entities` - what a typical entity looks like, and how many there are

`count` entities in all, split over the `types` by their `share` (equal shares if none is given). Each
type lists its attributes - an entity of the type has every one:

| attribute member | |
|---|---|
| `name` | the attribute's name |
| `type` | `Property` (default), `GeoProperty` (a Point), `Relationship`, `LanguageProperty`, `VocabProperty`, `ListProperty`, `JsonProperty` |
| `value` | a Property's value - a number varies per entity, anything else is copied |
| `observedAt` | `true`: the attribute carries an `observedAt` |
| `subAttributes` | how many Property sub-attributes it has |

Make the entity look like yours: the number of attributes, and how many are sub-attributed or
observed, decide how much every read and write handles.

### `subscriptions` - who is notified

A list of groups. Each group is `count` subscriptions, notifying a receiver that counts them:

| member | |
|---|---|
| `select` | `perEntity`: each one subscribes to ONE entity (of `type`, or of the first type) - entity 1, 2, 3, ... - the deployment where every device has its own subscriber. `byType`: each one subscribes to every entity of `type` - one change notifies all of them |
| `type` | the entity type |
| `watchedAttributes` | as in NGSI-LD: only a change of these notifies |
| `q` | as in NGSI-LD: only an entity that matches notifies |

`select` is what decides the fan-out - how many notifications one write sends - and so most of what a
notifying deployment costs.

### `load` - what the broker is asked, and how hard

| member | |
|---|---|
| `concurrency` | client connections at once (default 50) |
| `mix` | the share of each request: `patch` (an attribute update - `PATCH .../attrs`), `query` (`GET /entities?...`), `retrieve` (`GET /entities/{id}`), `create` (a new entity). The shares need not add up to 1 |
| `patch.attributes` | what a `patch` updates - per type, those of them it has, else its first Property |
| `query` | the query: `type`, `q`, `limit` |

`duration` is how long each measured run lasts (default 30 s), after a 5 s warm-up.

## What `make tune` does

1. **Checks the workload file** (`test/perf/tune/tuneGen.py`) before anything long starts.
2. **`make pgo`, trained on the workload** (`test/perf/tune/tuneTrain.sh`) instead of `pgoTrain.sh`:
   the instrumented broker runs the workload twice - once with each `--dbLockPrefer` - so the profile
   has the code of whichever wins.
3. **Measures the combinations** with the profile-guided build (`test/perf/tune/tuneMeasure.sh`):
   `--dbLockPrefer reads|writes` × glibc / jemalloc / tcmalloc, each `TUNE_REPEATS` times (2), the
   combinations alternated so the machine's drift spreads over all of them. Every run starts from an
   empty store: the entities, the subscriptions, the warm-up, the measured run.
4. **Writes `tune-result.json`** - the best combination, and every one:

```json
{
  "best": { "dbLockPrefer": "writes", "allocator": "tcmalloc_minimal", "brokerOptions": "--dbLockPrefer writes",
            "env": { "LD_PRELOAD": "/usr/lib/x86_64-linux-gnu/libtcmalloc_minimal.so.4" },
            "rps": 173721, "vsDefault": "+11.1 %" },
  "table": [
    { "dbLockPrefer": "writes", "allocator": "tcmalloc_minimal", "rps": 173721, "p99us": 6462,
      "notifiedPerRun": 0, "failedRequests": 0, "vsDefault": "+11.1 %", "runs": [171564, 175878] },
    "..."
  ]
}
```

`vsDefault` is against `--dbLockPrefer reads` with glibc's allocator - **with the profile trained on
your workload in both**. What the profile alone adds is the out-of-the-box image against `make tune`'s
build with its out-of-the-box settings (below). A combination whose requests FAILED (`failedRequests`)
does not win, however fast.

The knobs of a run: `TUNE_REPEATS` (2), `TUNE_TRAIN_DURATION` (20 s per lock preference),
`TUNE_BROKER_CPUS` / `TUNE_LOAD_CPUS` (`taskset` lists - pin the broker and the load apart on a big
machine).

## A Docker image of it

The image build takes the workload file - in the build context, i.e. inside the coraine checkout - and
runs `make tune` in the builder instead of `make pgo`:

```sh
cp my-workload.json coraine/tune/
docker build -f docker/Dockerfile --build-arg WORKLOAD=tune/my-workload.json -t my-coraine .
```

- The **profile** is built in.
- What won is kept in `/opt/seamware/etc/tune.env` (`TUNE_DBLOCKPREFER`, `TUNE_ALLOCATOR`), and only
  the allocator that won is installed in the image.
- The entry point (`coraine-start`) applies it - `CORAINE_DBLOCKPREFER`, `LD_PRELOAD` - and runs
  `coraine -fg`. Whatever `docker run -e` sets wins: `-e CORAINE_DBLOCKPREFER=reads` overrides the
  tuned lock preference, `-e LD_PRELOAD=` (empty) the tuned allocator - glibc's then.

⚠️ The measurement then runs on the **build** machine, inside the build. Build where the broker will
run, or on the same kind of machine.

## What it is worth

Two workloads, each measured three ways with the same harness (`tuneRun.sh`: an empty store, the
fixture, the subscriptions, a 5 s warm-up, 30 s measured; three runs each, alternated):

- **out of the box** - `make pgo` (the published image's build), its default settings
- **the tuned profile** - `make tune`'s build, the default settings: what the profile alone adds
- **tuned** - `make tune`'s build with the settings it chose

corDB on disk (`--dbDir`), release builds, AMD Ryzen 9 8940HX (32 threads, nothing pinned, nothing
else running), 2026-10-07. Requests/s - the mean of three runs, every run within 2 % of its mean; no
request failed:

**A deployment that subscribes** -
[`workload.example.json`](https://github.com/SEAMWARE/coraine/blob/main/test/perf/tune/workload.example.json):
10 000 entities (Vehicles and Parkings, four and three attributes), 105 subscriptions (one per
Vehicle for 100 of them, five on every Parking with a `q`), 80 % `PATCH`, 10 % queries
(`type=Vehicle&q=speed>50`, 20 at a time), 5 % retrieves, 5 % creates, 50 connections.

| | requests/s | | p99 |
|---|---:|---:|---:|
| out of the box | 6 239 | | 36.8 ms |
| the tuned profile | 6 383 | +2.3 % | 37.7 ms |
| **tuned** - `--dbLockPrefer writes`, tcmalloc | **9 165** | **+46.9 %** | **17.1 ms** |

Every query here reads 8 000 Vehicles under the store's read lock; with readers first, the
`PATCH`es queue behind them. Writers first lets them in - more throughput, and half the tail.

**An ingest** - [`workload.ingest.json`](https://github.com/SEAMWARE/coraine/blob/main/test/perf/tune/workload.ingest.json):
10 000 meters (four attributes, two of them observed), 70 % creates, 30 % `PATCH`, no subscriptions,
50 connections.

| | requests/s | | p99 |
|---|---:|---:|---:|
| out of the box | 155 001 | | 2.9 ms |
| the tuned profile | 154 789 | -0.1 % | 3.1 ms |
| **tuned** - `--dbLockPrefer writes`, tcmalloc | **174 374** | **+12.5 %** | **6.4 ms** |

More creates per second, at a longer tail: whether that trade is the right one is the deployment's
call - `tune-result.json` has every combination, its p99 beside its rate, to make it with.

- **The profile alone adds little** on these workloads (+2.3 %, -0.1 %): the general training already
  runs these paths. What a workload changes is the knobs.
- **Which knobs win differs.** `make tune`'s own comparison on the ingest: tcmalloc +11.1 %, glibc
  +7.1 %, jemalloc +5.7 % with writers first - and jemalloc -6.0 % with readers first. On the
  subscribing deployment jemalloc and tcmalloc are within 1 % of each other.
