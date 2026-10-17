# Building from source - in detail

The source layout, the features that compile out and what each one does, and the
choice of HTTP server. How to build and the make targets are in
[Building from source](building.md).

## Where things live

```
coraine/
├── src/
│   ├── app/coraine/        # main(), arg table, plugin wiring, NGSI-LD service map
│   ├── lib/
│   │   ├── plugin/          # pluginLoader.c + ApiPlugin.h  (the loader)
│   │   ├── db/              # DbDriver.h  (current-state plugin contract) + tenant
│   │   ├── troe/            # TroeDriver.h (temporal plugin contract) + dispatch
│   │   ├── serviceRoutines/ # NGSI-LD endpoint handlers
│   │   ├── linkedEntities/  # join / linked-entity support
│   │   ├── forwarding/      # distributed-ops (CSR) forwarding
│   │   └── metrics/         # Prometheus via corProm
│   └── plugins/
│       ├── currentState/    # mongoc   (DB plugins; corDB is its own repo, ../corDB)
│       ├── temporal/        # none, timescale  (TRoE plugins; corDB's in ../corDB/troe)
│       ├── api/admin/       # admin API plugin
│       └── shared/          # geoMatch.c etc. shared across plugins
├── test/funcTests/          # corTest functional tests
├── doc/                     # plugin architecture, functest coverage audit
├── CMakeLists.txt           # real build (feature flags, lib wiring)
└── makefile                 # convenience wrapper (release/debug/install/test/coverage)
```

## Compiling out what you don't need

`CMakeLists.txt` exposes `COR_FEATURE_*` options, toggled with `cmake -DCOR_FEATURE_X=OFF` (or
`coraine-build --features X=OFF`), in groups - a `# Group: <name> - <what>` line heads each, and
`coraine-build --list-features --json` reports every switch's group:

| group | switches |
|---|---|
| NGSI-LD API | `SUBSCRIPTIONS`, `REGISTRATIONS`, `AUTO_ENTITY_MAP`, `RESPONSE_BUDGET`, `ICU_COLLATION` |
| Snapshots | `SNAPSHOT_WRITE`, `SNAPSHOT_SUBSCRIPTIONS` |
| JSON-LD @context | `CONTEXT_HOSTING` |
| Geo | `GEOQ` |
| Security | `AUTH`, `POLICY_API` |
| Beyond NGSI-LD | `SERVICE_EXECUTION` |
| Protocols | `COR_PROTOCOL`, `TRANSPORTS` |
| Operations | `ADMIN_API`, `METRICS`, `HEALTH` |
| Stores and tools | `MONGOC`, `MIGRATE` |

All default ON except `ICU_COLLATION` and the observation/operation-space dispatch. Three are turned off
by what they need - `coraine-build --list-features --json` gives them as `requires`:

| switch | off when | why |
|---|---|---|
| `SNAPSHOT_SUBSCRIPTIONS` | `SUBSCRIPTIONS` or `SNAPSHOT_WRITE` is off | a subscription, notified by a Snapshot's writes |
| `METRICS` | `ADMIN_API` is off | the admin API plugin serves them (`/admin/metrics`, `/metrics`), nothing else does |
| `POLICY_API` | the HTTP server is the built-in one (`COR_HTTP_SERVER=builtin`; `HTTP_SERVER=mhd` in `requires`) | its port is a second listener, which only libmicrohttpd has |

The intent is a broker you can shrink to exactly the NGSI-LD you actually deploy —
no subscription engine on a read-only edge node, no geo, no Mongo. What each switch
removes is in the table below. Tenants, entity scope, `datasetId` and multi-type entities have no switch: they
are always in the broker, and compiled out each would save a few KiB.

`-DCOR_FEATURE_MONGOC=OFF` builds a Mongo-free tree (drop `libmongoc` from the
build host, run `--database corDB`).

`-DCOR_FEATURE_REGISTRATIONS=OFF` drops the Context Source Registration,
registration-subscription and EntityMap service routines, the forwarding
library, and both DB plugins' registration code — about 24 kB of `.text`.

`-DCOR_FEATURE_ICU_COLLATION=OFF` is the odd one out: it removes no routes and
no feature, it swaps the implementation behind one. `orderBy` on strings still
works, but through a dependency-free approximation instead of libicu — which
takes **three shared libraries and 39.2 MiB** off what the broker installs
(`libicudata` alone is 31.6 MiB, nine times the size of the broker, for a
collation table). Build `corNgsild` to match — `make -C ../corNgsild
COR_WITH_ICU=0 di` — or the broker links ICU against a library compiled for the
other path.

What is given up is measurable and small, and it is exactly three things:
punctuation no longer sorts before digits (`1one` before `_under`), a
non-ASCII letter takes its byte weight rather than its base letter's (`äpple`
after `zebra` instead of beside `apple`), and `collation=<locale>` is ignored.
Pure ASCII sorts identically either way. `orderby_collation_locale.test` is
written to fail on this build and say so, rather than to stay green about it.

`-DCOR_FEATURE_SUBSCRIPTIONS=OFF` drops the subscription CRUD, the
distributed-subscription notification receiver, the subscription code in both DB
plugins and the admin plugin's `subStats/flush` — about 19 kB. The two are
independent switches: a build can carry a complete Context Source Registration
API and no subscriptions at all.

In each case the routes keep their entry in the service table and answer

```
HTTP/1.1 501 Not Implemented

{
  "type":   "https://coraine.readthedocs.io/errors/NotAvailableInThisDeployment",
  "title":  "Not Available In This Build",
  "status": 501,
  "detail": "'POST /ngsi-ld/v1/csourceRegistrations' is not included in this build of coraine"
}
```

deliberately **not** a 404. A 404 says the resource is not there and invites the
client to fix its URL; this says the deployment declined the feature and the
client's move is a different deployment. A plugin's own routes answer the same
way — the admin plugin's `subStats/flush` is compiled out with subscriptions and
returns the same 501. The type URI is ours rather than an
ETSI one because TS 104-176 § 6.3.2 registers no error type for a build-time
omission — the one 501 in that table, `NoMultiTenantSupport`, is reserved for a
single feature. See spec-doubt #124.

Ask a binary what it carries, without starting it:

```console
$ coraine --version
coraine 0.5.0
features: SUBSCRIPTIONS=1 REGISTRATIONS=0 GEOQ=1 ...
```

and ask a running one for the whole picture with **`GET /build`** — the features
compiled in, the plugins this build produced against the ones actually loaded,
and the run-time settings that change what a client gets:

```json
{
  "product": "coraine",
  "version": "0.5.0",
  "build":    { "gitSha": "...", "builtAt": "...", "type": "Debug", "compiler": "GNU 15.2.0" },
  "features": { "REGISTRATIONS": false, ... },
  "plugins":  { "directory": "/opt/seamware/plugins", "built": [...], "loaded": {...} },
  "runtime":  { "distributed": false, "splitEntities": true, "httpEndpoint": "..." }
}
```

Three kinds of fact, kept apart because they change at three different moments —
a feature is fixed when the source was compiled, `loaded` is decided at startup
from a directory the binary does not own, and `runtime` changes with a restart.
That last one earns its place: a broker with `REGISTRATIONS` compiled **in** and
`--distributed` off accepts registrations and forwards nothing, and this is the
only place both switches are visible at once.

It is not under `/admin` on purpose — the admin API is itself a compile-time
feature, and the endpoint that reports what a build contains must not be one of
the things a build can leave out. `GET /version` is unchanged: product, version
and the linked-library commits. "What am I talking to" and "what can it do" are
different questions.

The functional suite asks the same question, of the `--version` line. A test that
needs a feature carries `# REQUIRE_FEATURE: <NAME>` and leaves the run set on a
build without it — 184 of the cases need `REGISTRATIONS` and 149 need
`SUBSCRIPTIONS` — and `# SKIP_FEATURE: <NAME>` marks the ones that can only run
on a build **without** it, which is how the 501s above are tested. Both markers
take a list, and `REQUIRE_FEATURE` means ALL of them: a registration
subscription needs `REGISTRATIONS SUBSCRIPTIONS`.

To build a reduced tree without turning your ordinary one into it:

```console
make di CMAKE_FEATURES=-DCOR_FEATURE_REGISTRATIONS=OFF BUILD_DEBUG=BUILD_DEBUG_MINIMAL
```

⚠️ CMake **caches** what it is given, per build directory. Re-running that
command with a different feature keeps the previous one off as well, which is
easy to miss because the build succeeds — check `coraine --version` (or
`GET /build`) rather than assuming. Give each combination its own directory.

### Every choice `coraine-build` offers

One heading per choice, as c³'s Compile page shows them; the bridges are `coraine-build --bridges` (`--list-bridges`). **Size** is what the choice changes in what a
build installs: the stripped programs and plugins (`coraine`, `coraine-import`, `plugins/`), and the
libraries they load that a bare `ubuntu:26.04` does not have
([Size on disk](performance.md#size-on-disk--what-installing-coraine-adds)). Measured 2026-10-13 on main
`c44d4b4e`, release builds, one switch turned from its default at a time against the default build. A
stripped file grows and shrinks in 4 KiB pages: a switch whose code is under a page reads 0 or 4 KiB.

#### Build mode: Release

`-O2`, traces compiled out (`--traceLevels` is not there), no training. `coraine-build`'s default
(`--release`).

#### Build mode: Profile-guided

`--pgo`. Built instrumented, trained on
[`test/perf/pgoTrain.sh`](https://github.com/SEAMWARE/coraine/blob/main/test/perf/pgoTrain.sh) - perfRun's
request shapes on corDB in RAM and on disk with history, a write that notifies, the three-broker
forwarding chain over HTTP and cor:// - and rebuilt with the profile, the libraries with it. +1-10 % per
core against Release ([Profile-guided](performance.md#profile-guided-make-pgo)). Needs `curl`, `wrk` and
`python3`; libmicrohttpd only. Size: none.

#### Build mode: Tuned for a workload

`--tune <workload.json>`. Profile-guided, trained on **your** workload instead of the general mix, and two
more knobs measured on it - the ones that depend on the workload, not on the deployment:

- **the profile** - the code laid out for the paths your workload runs;
- **the memory allocator** - glibc's `malloc`, jemalloc or tcmalloc (`LD_PRELOAD`, no rebuild); on
  concurrent creates into a durable corDB jemalloc is +11 %, tcmalloc +18 %;
- **whom corDB's store lock lets in first** (`--dbLockPrefer reads|writes`) - writers first: creates +5 %,
  batch creates +15 %, merges +7.5 %, reads and notifying PATCHes -3-6 %.

The workload file says where the entities live, what a typical entity looks like and how many there are,
who is notified, and what the broker is asked and how hard
([The workload file](extreme-performance.md#the-workload-file)). The best of each knob goes to
`etc/tune.env` in the install (`CORAINE_*` and `LD_PRELOAD`). ~15 minutes; needs what Profile-guided
needs, and jemalloc and tcmalloc to compare them (`libjemalloc2`, `libtcmalloc-minimal4`; a missing one
is left out). [Extreme performance](extreme-performance.md).

#### Build mode: Debug

`--debug`. `-g`, not optimised, traces compiled in: `--traceLevels` works. For reproducing a problem
report, not for production.

#### HTTP server: libmicrohttpd

`--http-server mhd`, the default. libmicrohttpd 1.0.10, and GnuTLS behind it: nine libraries, 7.29 MiB.
Everything builds.

#### HTTP server: corHttp (built in)

`--http-server builtin`. The broker's own epoll server, no external library. Size: **-7.3 MiB** - nine
libraries less than libmicrohttpd, for 12 KiB of corHttp. One listener: `POLICY_API` (a port of its own) is not built. Not with
Profile-guided or Tuned yet. [Choosing the HTTP server](#choosing-the-http-server).

#### Store: corDB only

`--features MONGOC=OFF`. Entities in the broker's own memory, on disk with `--dbDir`. `mongoc.so` is not
built and its libraries not loaded. Size: **-6.8 MiB** - `mongoc.so` 145 KiB, libmongoc 5.59 MiB, libbson
1.08 MiB; libsasl2 (0.10 MiB) goes too when `timescale.so` is not installed (libpq needs it).

#### Store: corDB + mongoc

The default: `--database mongoc` too, entities in a MongoDB.

#### Bridge: MQTT

`--bridges mqtt` (default on). `mqtt.so`: NGSI-LD over MQTT topics - a Channel maps a topic to an entity's
attributes, both ways ([Bridges and Channels](bridge-channels.md)). Needs libmosquitto (`libmosquitto-dev` to build). Size:
**+0.13 MiB** - the plugin 27 KiB, libmosquitto 0.10 MiB.

#### Bridge: Modbus

`--bridges modbus` (default on). `modbus.so`: values between Modbus TCP registers and entities. No library.
Size: **+0.03 MiB**.

#### Bridge: DDS

`--bridges dds`. `dds.so`: topics, services and actions between DDS and entities ([DDS](dds.md)). Needs
eProsima Fast DDS and the DDS Enabler installed; without them `coraine-build` says so and does not build.
Size: **+23.6 MiB** - the plugin 123 KiB, and twelve libraries (Fast DDS, Fast CDR, the DDS Enabler and DDS
Pipe, yaml-cpp, tinyxml2, foonathan memory) 23.5 MiB: five times the broker.

#### Bridge: CKAN

`--bridges ckan`. `ckan.so`: an open data portal's datasets (CKAN's API), discovered and read into
entities, through DataChannels (`DATA_CHANNELS`). No library. Only when the source carries it. Size:
**+0.03 MiB**.

#### `SUBSCRIPTIONS`

Subscriptions and their notifications. Off: the 7 subscription routes answer 501, the
subscription code leaves both store plugins, `SNAPSHOT_SUBSCRIPTIONS` goes with it. Size: -64 KiB.

#### `REGISTRATIONS`

Context Source Registrations and what they drive: distributed operations (forwarding), registration
subscriptions, EntityMaps. Off: 15 routes answer 501, the forwarding library and both
store plugins' registration code go. Size: -72 KiB.

#### `AUTO_ENTITY_MAP`

Automatic EntityMaps: a query of more than one page freezes its set of matches and serves its pages from
it - distributed queries by default, local ones too with `--autoEntityMaps all`. Off: no
`--autoEntityMaps`; pages by `offset` / `limit`; a map only when a client asks for one. Size: -4 KiB.
[EntityMaps](installation.md#entitymaps).

#### `RESPONSE_BUDGET`

The byte budget of a query (`--maxResponseSize`): a shorter page, or 403 TooManyResults. Off: no budget.
Size: -12 KiB. [Response size](installation.md#response-size).

#### `ICU_COLLATION`

Off by default. On: `orderBy` on strings by ICU root collation (§ 7.6.2.1), and `collation=<locale>`.
Off: ASCII sorts the same; punctuation does not sort before digits, a non-ASCII letter sorts by its bytes,
`collation=` is ignored. Size, on: **+39.2 MiB**, three libraries (`libicudata` 31.6 MiB). Needs
`libicu-dev` to build.

#### `SNAPSHOT_WRITE`

Writes on a Snapshot (`NGSILD-Snapshot` on an entity or temporal write, § 7.9.2). Off: such a write answers
422 - Snapshots are read-only. Size: under a page (568 bytes of code).

#### `SNAPSHOT_SUBSCRIPTIONS`

Subscriptions on a Snapshot, notified by its writes. Off: a subscription write naming a Snapshot answers
422. Off by itself when `SUBSCRIPTIONS` or `SNAPSHOT_WRITE` is. Size: under a page (1 491 bytes).

#### `CONTEXT_HOSTING`

The `/jsonldContexts` API and its store. Off: its 4 routes answer 501; an inline @context and the core
context work as before. Size: -20 KiB.

#### `GEOQ`

Geo-queries (`georel`, `geometry`, `coordinates`, and `geoQ` in a query body, a subscription or a Service
Execution object). Off (`COR_WITH_GEOQ=0` - `coraine-build` maps the switch to it, `CMAKE_FEATURES` refuses it):
the geo matcher (`shared/geoMatch.c`) is built without GEOS in `mongoc.so` and in `corDB.so`, neither links
libgeos_c, and a request asking for a geo-query is answered **501** "not in this build" - not as if nothing
matched. GeoProperty data is stored, served and queried as before. corDB no longer refuses a degenerate or
self-intersecting polygon (the check that mirrors a 2dsphere index is GEOS's). Size: **-3.19 MiB** - GEOS
(`libgeos_c` 1.20.5 and `libgeos` 3.14.1, 3.17 MiB) no longer loaded, `mongoc.so` -16 KiB, `corDB.so`
-12 KiB, `ramDB.so` -4 KiB; the broker +8 KiB, its 501s (measured 2026-10-14).

#### `AUTH`

Auth plugins: who sent a request and whether it may (`--auth`; `fdsc.so`, the FIWARE Data Space
Connector's tokens and policies). Off: no `--auth`, every request let in, `fdsc.so` not built. Size:
-113 KiB. [Authorization](authorization.md).

#### `POLICY_API`

The ODRL PAP's `/policy` API on a port of its own (`--policyApiPort`) - the policies `fdsc.so` reads,
held by the broker. Off: no `--policyApiPort`. Needs libmicrohttpd. Size: -9 KiB.
[Authorization § 7](authorization.md#7-the-policy-api---the-broker-as-its-own-pap).

#### `SERVICE_EXECUTION`

Service registrations, executions and executors (ETSI GR CIM-055). Off: 17 routes answer 501. Size:
-169 KiB.

#### `DATA_CHANNELS`

DataChannels: a bridge endpoint whose samples are records (a CKAN row), each made into entities by a mapping
([DataChannels](bridge-channels.md#datachannels)). Off: a `dataChannels` section is warned about and
ignored, `GET /ngsi-ld/v1/dataChannels` answers 501, and the CKAN bridge is not offered. Size: -14,720 bytes.

#### `CHANNEL_STATUS_IN`

`BridgeBroker.channelStatusIn` (bridge ABI 12): a bridge plugin says, at any time, whether a Channel carries,
and why not - `status` and `statusReason` on `GET /ngsi-ld/v1/channels`, a dormant one a warning in the log.
Off: the slot is NULL, and a Channel the plugin refuses shows the broker's own reason. Size: -576 bytes.

#### `CHANNEL_CHECKS`

`BridgeDriver.payloadCheck` (bridge ABI 13): a bridge plugin may refuse what a client writes to an attribute
one of its Channels or DataChannels maps, before anything is sent or stored
([the payload check](bridge-channels.md#the-payload-check)). Off: no write is checked, and
`payloadsRefused` is not shown. Size: -3,449 bytes.

#### `COR_PROTOCOL`

cor://, the binary NGSI-LD protocol: a listener next to HTTP (`--corPort`), and forwarding to a
registration whose endpoint is `cor://` ([cor-protocol.md](cor-protocol.md)). Off: no `--corPort`; a cor://
registration is not forwarded to (`NGSILD-Warning: 299`, the scheme not supported); corRest is built
without its cor:// server and client. The cor format stays - corDB's log is written in it. Set with
`COR_WITH_COR_PROTOCOL=0` (`make`) or `--features COR_PROTOCOL=OFF` (`coraine-build`), not as a cmake `-D`
alone. Size: -24 KiB.

#### `TRANSPORTS`

Transport plugins - the API over another protocol (`--transports`; `ws.so`, WebSocket). Off: no
`--transports`, `ws.so` not built. Size: -38 KiB.

#### `ADMIN_API`

The admin API plugin (`admin.so`). Off: not built; `METRICS` goes with it. Size: -40 KiB.

#### `METRICS`

Prometheus metrics (`/admin/metrics`, `/metrics`). Off: none. Size: -17 KiB.

#### `HEALTH`

The health port (`--healthPort`): `/live`, `/ready` and a report, on a thread of its own, and the store
plugins' `ping`. Off: no `--healthPort`. Size: -8 KiB.

#### `MONGOC`

The mongoc store plugin: as [Store: corDB only](#store-cordb-only).

#### `MIGRATE`

`coraine-import`: another broker's database written into coraine's stores
([migration.md](migration.md)). Off: not built; the broker carries no import code either way. Size:
-1.3 MiB.

### ⚠️ Building a stripped-down broker is at your own risk

Every `COR_FEATURE_*` option can be set to `OFF` and most of them will build.
That is not the same as most of them working. Below is what each one actually
does today, measured by building the broker fourteen times with one feature off
at a time and comparing the result against a full build.

**These twelve remove code from the broker and are covered by the test suite:**

| flag | `.text` removed | endpoints affected |
|---|---|---|
| `COR_FEATURE_REGISTRATIONS=OFF` | 23,936 bytes | 15 routes answer 501 |
| `COR_FEATURE_SUBSCRIPTIONS=OFF` | 18,880 bytes | 7 routes answer 501 |
| `COR_FEATURE_TRANSPORTS=OFF` | 11,348 bytes, and `ws.so` is not built | no `--transports`; `GET /ngsi-ld/v1/ws` is no upgrade |
| `COR_FEATURE_AUTH=OFF` | 6,313 bytes (release build), and `fdsc.so` is not built | no `--auth` - every request is let in ([Authorization](authorization.md)) |
| `COR_FEATURE_POLICY_API=OFF` | 6,528 bytes (release build) | no `--policyApiPort` - the broker serves no `/policy` API ([Authorization § 7](authorization.md#7-the-policy-api---the-broker-as-its-own-pap)) |
| `COR_FEATURE_SERVICE_EXECUTION=OFF` | 77,410 bytes | 17 routes answer 501 |
| `COR_FEATURE_SNAPSHOT_WRITE=OFF` | 568 bytes | a write naming a Snapshot (`NGSILD-Snapshot`) answers 422 - snapshots are read-only |
| `COR_FEATURE_SNAPSHOT_SUBSCRIPTIONS=OFF` | 1,491 bytes | `/subscriptions` naming a Snapshot (`NGSILD-Snapshot`) answers 422 for a write; a read lists the live tenant's. Off by itself when `SUBSCRIPTIONS` or `SNAPSHOT_WRITE` is |
| `COR_FEATURE_HEALTH=OFF` | 5,312 bytes (and 432 from `mongoc.so`, 782 from `timescale.so`) | no `--healthPort` - the health port, its pingers and the `ping` of the `mongoc` and `timescale` plugins are not built |
| `COR_FEATURE_RESPONSE_BUDGET=OFF` | 5,056 bytes (and 256 from `mongoc.so`, 1,472 from `timescale.so`; release build. `corDB.so`, built in its own repository, keeps its counting, which nothing then turns on) | no `--maxResponseSize` - no byte budget on an entity query, a temporal query or the forwarded part of a distributed one ([Response size](installation.md#response-size)) |
| `COR_FEATURE_AUTO_ENTITY_MAP=OFF` | 3,680 bytes (release build) | no automatic EntityMaps and no `--autoEntityMaps` - a query paginated past its first page is paged by `offset` / `limit`, locally and forwarded; a map only when a client asks for one (`--entityMapMemory` still caps those) ([EntityMaps](installation.md#entitymaps)) |
| `COR_WITH_GEOQ=0` (`GEOQ`) | in `mongoc.so`, `corDB.so` and `ramDB.so` 33 KiB stripped, and GEOS: libgeos_c and libgeos are not loaded (3.17 MiB); the broker grows 8 KiB (its refusals) | no geo-queries - `georel`, a `geoQ` in a query body, a subscription or a Service Execution object, ordering by distance are answered 501; corDB accepts any polygon |
| `COR_FEATURE_DATA_CHANNELS=OFF` | 14,720 bytes (release build) | no DataChannels - a bridge's `dataChannels` section in `--bridgeConfig` is warned about and ignored, `GET /ngsi-ld/v1/dataChannels` answers 501 ([DataChannels](bridge-channels.md#datachannels)) |
| `COR_FEATURE_CHANNEL_STATUS_IN=OFF` | 576 bytes (release build) | `BridgeBroker.channelStatusIn` is NULL: a bridge plugin cannot set a Channel's status and statusReason after `channelAddInfo`; a refused Channel shows the broker's own reason |
| `COR_FEATURE_CHANNEL_CHECKS=OFF` | 3,449 bytes (release build) | no payload check - a bridge's `payloadCheck` is never called, every client write to a mapped attribute goes on to the transport and the store as before ([the payload check](bridge-channels.md#the-payload-check)) |

**These two leave the broker unchanged and drop a plugin**, which is the whole
of their effect — the broker is a plugin loader and it simply has one fewer to
load:

| flag | effect |
|---|---|
| `COR_FEATURE_MONGOC=OFF` | `mongoc.so` is not built; run `--database corDB` |
| `COR_FEATURE_ADMIN_API=OFF` | `admin.so` is not built |

**This one leaves the broker unchanged and drops a program:**

| flag | effect |
|---|---|
| `COR_FEATURE_MIGRATE=OFF` | `coraine-import` is not built ([migration.md](migration.md)) - the broker carries no import code either way |

**Every switch builds**, turned from its default alone: the nightly's `switches` job builds each
(`.github/scripts/every-switch.sh`) and checks that `--version` says so. `CONTEXT_HOSTING=OFF`: its 4
routes answer 501 (`jsonld_contexts_not_in_this_build`); `METRICS=OFF`: no `/admin/metrics`, `/metrics`.

**What to check.** After any reduced build, ask the binary what it thinks it is
rather than assuming the flag took:

```console
$ coraine --version          # before it even starts
$ curl localhost:1026/build  # features, plugins built vs loaded, runtime
```

and remember that a `false` in that list means *the flag was set*, not
*the code is gone* — for the nine above they are different statements.

**If you hit trouble**, please open an issue at
[github.com/SEAMWARE/coraine/issues](https://github.com/SEAMWARE/coraine/issues)
saying which flags you set and what happened, and paste the `GET /build`
output. Reduced builds are a direction this project is committed to, and the
combinations nobody has tried are exactly the ones worth hearing about.

### Choosing the HTTP server

```console
cmake -DCOR_HTTP_SERVER=mhd        # libmicrohttpd (the default)
cmake -DCOR_HTTP_SERVER=builtin    # corHttp, no external HTTP dependency
```

From a package: `coraine-build --http-server builtin` (not with `--pgo` / `--tune` yet). With `builtin`,
`POLICY_API` is off: its port is a second listener, and corHttp has one.

Not a `COR_FEATURE_*` boolean, because those answer "is this feature in the
build" and the HTTP server is always in it — what varies is which one. The value
reaches CMake **and** corRest's own make (the HTTP server lives in corRest,
which builds with plain make and knows nothing of CMake options), and it decides
whether `libmicrohttpd` is on the link line at all, so `ldd coraine` is the
check that it took. A running broker reports it as `build.httpServer` on
`GET /build`.

`builtin` selects **corHttp**, a sibling repo: an HTTP/1.1 server on
edge-triggered epoll loops, depending on nothing but corAlloc and libc. A request
runs on the loop that read it - inline, or as a coroutine when it can wait
([Coroutines](coroutines.md)) - and the wire format it emits is byte-for-byte the
one libmicrohttpd produced, because several hundred functional tests compare
captured responses line by line.

It runs **several loops sharing one port**: loop 0 accepts every connection and
deals them out in turn, through each loop's own queue (`corHttpAcceptShare`) - the
kernel's own `SO_REUSEPORT` hash splits a handful of connections unevenly.
`--httpLoops` sets how many; the default, `0`, means one per core capped at four. Capped, because
past four the box saturates and the extra throughput is paid for in tail latency
— on 8 cores, eight loops bought 16% over four and made p99 seven times worse.
Per-core, because four loops on a single core are four loops competing for it,
which measures slower than one. `--httpLoops` is accepted on a libmicrohttpd
build too, and ignored there: that server has a thread per connection and no
loop of ours to multiply, and a deployment should not have to know which server
its binary carries in order to write a command line.

Two things it does not do, and both are deliberate rather than pending:

- **No TLS server.** The broker never serves HTTPS (it is put behind a proxy
  that does), so this costs exactly one thing: the test notification receiver
  `corTestClient` cannot serve HTTPS either, and the one functional test that needs
  that carries `REQUIRE_HTTPSERVER: mhd`.
- **No HTTP pipelining.** A second request arriving in the same packet as the
  first is dropped rather than answered. No client in use here pipelines — curl
  does not, and neither browsers nor the ETSI suite's HTTP library do — and
  supporting it properly means driving the event loop from the response side.

**Both servers are tested, but not in the same place.** A pull request builds and
tests one of them — `mhd`, the default — because doubling a two-minute check to
answer a question that changes once a month is the wrong trade. The **nightly**
crosses its valgrind matrix and its ETSI job with `COR_HTTP_SERVER`, so `builtin`
gets the whole functional suite under valgrind and the whole conformance suite,
every night that something merged.

Why bother: `libmicrohttpd` is ~180 kB of mapped code, about 21% on top of the
broker's own, for a library coraine uses 32 of the 81 exported symbols of — and
it is the last third-party runtime dependency besides libc and OpenSSL once the
optional features are compiled out. It is also a tarball fetched from
ftp.gnu.org and built from source.

**Which libmicrohttpd.** The images and CI build **1.0.10** with corLibs'
`tools/mhd/mhdBuild.sh` - the CI image, the published image (`docker/vendor-libs.sh`
stages the directory) and the bare runners (`.github/scripts/ci-env.sh`). It
accepts a release tarball only with the SHA-256 pinned in the script (each pin
taken from a tarball whose GPG signature was checked), and applies the patches in
`tools/mhd/patches/<version>/`. On a workstation:

```console
~/git/corLibs/tools/mhd/mhdBuild.sh --sudo                 # /usr/local, with TLS (the https tests need it)
~/git/corLibs/tools/mhd/mhdBuild.sh --sudo -- --disable-https
```

A distribution's `libmicrohttpd-dev` builds and runs coraine as well; 1.0.4 and
1.0.5 fixed HTTP request smuggling (CWE-444), so a release before 1.0.5 is not one
to put behind a gateway.

### The optional runtime dependencies

MQTT notifications are the `mqtt.so` bridge plugin: a broker started without
`--bridges mqtt` never maps libmosquitto.
