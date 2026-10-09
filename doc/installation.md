# Installation & Administration Guide

## What coraine needs

coraine is a single binary plus the plugins it loads at startup. A working broker
with the in-memory store needs **no external service at all**. The MongoDB backend
needs a MongoDB server; the TimescaleDB temporal backend needs PostgreSQL with the
TimescaleDB extension.

### System packages (Debian / Ubuntu)

| Need | Package |
|------|---------|
| HTTP server | `libmicrohttpd-dev` |
| TLS | `libssl-dev` |
| MQTT bridge plugin (`mqtt.so`, notifications to `mqtt://`) | `libmosquitto-dev` |
| Geo queries | `libgeos-dev` |
| `orderBy` collation | `libicu-dev` |
| MongoDB driver (`mongoc` plugin) | mongo-c **v2** (`mongoc2.pc` via pkg-config) |
| TimescaleDB plugin | `libpq-dev` |
| Toolchain | `cmake build-essential` |

The mongo-c **v2** driver is the most common build snag — it is packaged by few
distributions and is normally compiled from source. Build without MongoDB support
with `cmake -DCOR_FEATURE_MONGOC=OFF` and run with `--database corDB`.

## Install from source

The dependency stack is a set of sibling repositories. The `corLibs` umbrella clones
and builds all of them at their pinned versions:

```sh
git clone git@github.com:SEAMWARE/corLibs.git
./corLibs/bootstrap.sh
cd coraine
make i          # release build + install
```

`make install` writes:

- the broker to `/usr/local/bin/coraine`
- the plugins to `/opt/seamware/plugins/{db/currentState,troe/temporal,api}/`
- the provenance file to `/opt/seamware/etc/contextSourceExtras.json`

Run with sufficient privileges, or pre-create those directories.

[Building from source](building.md) has every system package and all the make
targets; [its detail page](building-details.md) the source layout and how to
compile features out. This page covers the common case only.

## Install with apt

Two Debian packages, for **Ubuntu 26.04**, **Ubuntu 24.04 LTS** and **Debian 13 (trixie)**, on
**amd64** and **arm64**:

| Package | What it holds |
|---------|---------------|
| `coraine` | the broker (`/usr/bin/coraine`), `coraine-import`, every plugin but the DDS bridge (`/opt/seamware/plugins`), `/opt/seamware/etc/contextSourceExtras.json`, the systemd unit `coraine.service` and its options file `/etc/default/coraine` |
| `coraine-dev` | the exact source of the same version - coraine and every Cor-Lib at the commits built - in `/usr/share/coraine/src/coraine-src.tar.xz`, the toolchain and libraries to build it (as dependencies), and `coraine-build` |

The packages are built by the GitHub workflow `Packages` (`.github/workflows/packages.yml`) - for every
release, and on demand - and kept as **artifacts of that run**, one per distribution and architecture
(`debs-<dist>-<arch>`). A signed apt repository is **coming**; until then, install a downloaded file:

```sh
sudo apt install ./coraine_0.4.0+ubuntu24.04_amd64.deb      # apt resolves the dependencies
```

Versions: `0.4.0+<dist>` for a release, `0.4.0~git<YYYYMMDD>.<sha8>+<dist>` for a build of any other
commit (it sorts before `0.4.0`). `<dist>` is `ubuntu26.04`, `ubuntu24.04` or `debian13`.

### The coraine package

- **Dependencies** come from the distribution: OpenSSL, GEOS, libmicrohttpd, libmosquitto, libpq, zstd -
  and, on Ubuntu 26.04, the MongoDB C driver v2 (`libmongoc2-2`). Ubuntu 24.04 and Debian 13 package
  only v1 of that driver, so there the package carries v2 itself, privately, in `/opt/seamware/lib`: the
  `mongoc` plugin finds it through its RUNPATH, and the system's `libmongoc-1.0` is not touched.
- **The service** is installed but **not enabled and not started**: the broker listens on every
  interface and has no authentication of its own. Start it when it is configured:

  ```sh
  sudoedit /etc/default/coraine
  sudo systemctl enable --now coraine
  journalctl -u coraine -f
  ```

  It runs `coraine -fg` as the system user `coraine`, with the options in `/etc/default/coraine`
  ([Environment variables](#environment-variables)). Out of the box: `corDB` on disk in
  `/var/lib/coraine/db`, no temporal history, port 1026. `systemctl stop` is a clean stop (SIGTERM -
  corDB syncs its log and snapshots, [corDB on disk](#cordb-on-disk)). An upgrade restarts a running
  broker.
- **Removing** the package stops the broker; **purging** it removes `/etc/default/coraine`, but keeps the
  data in `/var/lib/coraine` and the user `coraine`.

### coraine-dev: build your own

```sh
coraine-build --list-features
coraine-build --features REGISTRATIONS=OFF,SERVICE_EXECUTION=OFF ~/coraine-small
. ~/coraine-small/install/env
coraine -fg --database corDB
```

`coraine-build [options] <dir>` unpacks the source into `<dir>` and builds there, as any user; nothing is
written outside `<dir>`. The result is in `<dir>/install` (`bin/`, `plugins/`, `etc/`, and `env`, which
points `PATH`, `SEAMWARE_PLUGIN_DIR` and `CORAINE_CONTEXTSOURCEEXTRAS` at it).

| Option | Build |
|--------|-------|
| `--release` (default) | optimised, traces compiled out |
| `--debug` | traces compiled in (`--traceLevels`) |
| `--pgo` | profile-guided: trained on the measured request shapes, as the packages and the image ([Performance](performance.md)) |
| `--tune <workload>` | profile-guided on your workload, the knobs measured on it ([Extreme performance](extreme-performance.md)) |
| `--features NAME=ON\|OFF,...` | the `COR_FEATURE_*` switches ([Building - details](building-details.md)) |

The packages are made by the scripts in `packaging/deb/` (`source.sh`, `build.sh`, `test-install.sh`,
`test-dev.sh`, `publish.sh`), each of which runs by hand in a container of the target distribution.

## Install with Docker

See [the docker README](https://github.com/SEAMWARE/coraine/blob/main/docker/README.md).
Build the image locally with the `Dockerfile` there; published images will land at
`quay.io/seamware/coraine:<version>-<date>-<sha>` - one immutable tag per merge, never `latest`.

## Running

The default listen port is **1026**, the default plugins are `mongoc` (current state)
and `none` (temporal), and no API plugins are loaded.

```sh
# In-memory, pretty JSON, admin API on - no external service required
coraine --database corDB --troe none --apiPlugins admin -pp 2

# MongoDB on a custom port
coraine --port 1027 --database mongoc --dbHost localhost

# Everything, including the selected plugins' own options
coraine --apiPlugins admin --database mongoc --usage
```

## Configuration

Every setting is a command-line option. **`coraine --usage`** (`-u`) prints the full
list, and `-U` prints it with descriptions; the list changes with the plugins you
select, because a plugin contributes its own options (for example `--dbHost`,
`--dbPort`, `--dbUser` come from the `mongoc` plugin).

### Core options

| Option | Default | Meaning |
|--------|---------|---------|
| `--port` / `-p` | 1026 | TCP listen port |
| `--database` / `-db` | `mongoc` | current-state plugin (short name or path) |
| `--troe` / `-troe` | `none` | temporal plugin (`none` disables history) |
| `--troeSync` / `-troeSync` | off | record temporal writes before the response, so a temporal read sees them at once |
| `--apiPlugins` / `-api` | — | comma-separated API plugins (e.g. `admin`) |
| `--pretty-print` / `-pp` | 0 | JSON indentation (0 = compact) |
| `--connectionPoolSize` / `-cps` | 32 | HTTP server thread-pool size |
| `--noInline` | off | hand every request to a worker thread. By default, with `corDB`, a request that waits on nothing runs on the thread that read it - about twice the throughput for retrieves and small queries. Metrics: `ngsild_requests_inline_total`, `ngsild_requests_worker_total` |
| `--memoryLimit` | 85% of the container's limit | memory budget in MiB - see [Memory budget](#memory-budget) |
| `--healthPort` | — (off) | TCP port for the health probes - see [Health port](#health-port) |
| `--healthStallTimeout` | 30 | seconds a request may be in flight, with no request finishing, before `GET /live` answers 503 |
| `--maxRequestSize` / `-mrs` | 2 | max request body, MiB (0 = no cap, § 6.3.2) |
| `--maxResponseSize` | 1/16 of the memory budget; none without one | byte budget of an entity query, MiB (0 = no budget) - see [Response size](#response-size) |
| `--entityMapMemory` | 64 | memory all EntityMaps may hold together, MiB (0 = no cap, and no automatic EntityMaps) - see [EntityMaps](#entitymaps) |
| `--distributed` / `-dist` | off | forward operations to registered Context Sources |
| `--noSplitEntities` | off | each entity lives wholly at one source |
| `--httpEndpoint` / `-he` | auto | externally reachable base URL |
| `--csourceAlias` | endpoint authority | alias used in `Via` loop detection |
| `--defaultUserContext` / `-duc` | — | default user `@context` URL |
| `--corsOrigin` / `--corsMaxAge` | — / 86400 | CORS origin and preflight cache |
| `--distOpTimeout` / `-dtmo` | 5000 | HTTP client timeout (ms) for forwards, notifications, `@context` downloads |
| `--cooldownMillis` / `-cms` | 30000 | endpoint cooldown after a delivery failure |
| `--notifyValueChangeOnly` / `-nvco` | off | suppress value-neutral update notifications |
| `--insecureNotif` | off | accept self-signed certificates on TLS notifications |
| `--high-precision` / `-hp` | off | nanosecond timestamps instead of microsecond |
| `--asyncSnapshot` | off | run snapshot queries in the background |
| `--subStatsFlushInterval` / `-ssfi` | 60 | subscription-statistics flush interval (s) |
| `--contextSourceExtras` / `-csx` | `/opt/seamware/etc/contextSourceExtras.json` | JSON rendered verbatim on `/info/sourceIdentity` |
| `--high-availability` / `-ha` | — | keep the caches in step with the other instances (`mongo` = change streams; needs the `mongoc` DB **and** a replica set) - see [High Availability](high-availability.md) |
| `--version` / `-V` | — | print the version and exit |
| `--traceLevels` / `-t` | — | trace levels for debugging |

### corDB on disk

`corDB` keeps the store in the broker's memory. With **`--dbDir <directory>`** it also keeps
it on disk and survives a restart: every write appends its effect to a log, synced
every 100 ms, with a snapshot as the log grows. A clean stop (SIGTERM) loses nothing, and
neither does `kill -9`, a crash or an OOM kill: the log is memory-mapped, so a record is in
the kernel's page cache before the response. A power cut loses at most the last unsynced
interval.

| Option | Default | Meaning |
|--------|---------|---------|
| `--dbDir` | none: in RAM only | the directory - one subdirectory per tenant, and the [storage format](#storage-format) |
| `--dbSync` | `interval` | `interval`: synced every `--dbSyncInterval` ms; `request`: a write answers once its record is on the disk; `none`: written, never synced |
| `--dbSyncInterval` | 100 | ms between two syncs |
| `--dbSnapshotEvery` | 64 | MiB of log after which a tenant is snapshotted (and at least as much as its last snapshot) |
| `--dbCompress` | off | the snapshots and the finished log segments compressed (zstd, loaded only then - libzstd is already in the image); the open segment never. A store written with it is read with or without it |

What it costs: [Performance](performance.md), "corDB on disk". How it works:
[corDB's persistence](https://github.com/SEAMWARE/corDB/blob/main/doc/persistence.md).

### ramDB - corDB in RAM only

**`--database ramDB`** is corDB built without the disk: the same store and the same API, nothing
written anywhere. It is for a deployment that wants the fastest store and can afford to lose it -
pub/sub, where a broker passes notifications on and what it holds is only ever the latest value, or
a cache in front of another system. **A restart starts empty**, by design, and nothing warns about it
at runtime: choose it knowing that.

- No disk options: `--dbDir` is an unknown option with ramDB, not one quietly ignored.
- No `--troe corDB`: it is refused (corDB keeps history inside its own store, on disk). The other
  TRoE choices work: `--troe none` (the default), `--troe timescale` for history in PostgreSQL, or
  `--troe ramDB` - a ring of the latest TRoE events in RAM, a dev/test tool today.
- Everything else - queries, subscriptions, registrations, tenants, geo-queries - is corDB's.

### Environment variables

**Every** command-line option can also be given as an environment variable, named
`CORAINE_` + the long option in upper case, with `-` becoming `_`:

| Option | Environment variable |
|--------|----------------------|
| `--port` | `CORAINE_PORT` |
| `--database` | `CORAINE_DATABASE` |
| `--troe` | `CORAINE_TROE` |
| `--apiPlugins` | `CORAINE_APIPLUGINS` |
| `--maxRequestSize` | `CORAINE_MAXREQUESTSIZE` |
| `--maxResponseSize` | `CORAINE_MAXRESPONSESIZE` |
| `--high-precision` | `CORAINE_HIGH_PRECISION` |
| … | … |

`coraine -U` (extended usage) prints the environment-variable name of every option
alongside its type and default, so the list never has to be maintained by hand.
A command-line argument overrides the environment variable.

Two variables are read outside that mechanism. They move the installation's directories, and they
name no file:

| Variable | Default | Meaning |
|----------|---------|---------|
| `SEAMWARE_PLUGIN_DIR` | `/opt/seamware/plugins` | base directory plugin short names resolve against (read by the plugin loader, before the arguments are parsed) |
| `SEAMWARE_ETC_DIR` | `/opt/seamware/etc` | where the broker looks for `bridges.json` when `--bridgeConfig` is not given, and for `contextSourceExtras.json` when `--contextSourceExtras` is not given. A file missing from there is not an error |

## Storage format

Every store records the version of the format its data is written in. At start, a broker checks
it in every store it opens:

| Store | Where the version is | 0.5.0 writes |
|-------|----------------------|------------|
| `mongoc` | `<database>.metadata`, document `{ _id: "storageFormat", version: <n> }` - in the default tenant's database and in every tenant's | 1 |
| `corDB` with `--dbDir` | `<--dbDir>/_storageFormat`: the number, as text | 1 |
| `timescale` (TRoE) | the schema version: `max(version)` of `troe_schema_version`, in every tenant's database | 3 |

- **No version recorded, no data**: a new store - this build's version is written.
- **No version recorded, data**: a store written before the version was recorded (`mongoc`:
  coraine 0.4.x or earlier). It is read as it is, and this build's version is written.
- **A version up to this build's**: the store is used. A lower version is upgraded and this build's
  is written.
- **A version above this build's**: the store was written by a newer release. **The broker does not
  start**: it exits with 1, and the error names the store, the version found and the newest this
  build knows. Run the release that wrote it, or a newer one. A tenant's `mongoc` database in a newer
  format that appears while the broker runs is not used: the request that would create the tenant is
  answered 500.

`mongoc` version 1 is the format of coraine 0.5.0: system timestamps once per entity and attribute
types once per entity. coraine 0.4.x reads that format wrong - attributes that 0.5.0 has written
come back without `type` and `createdAt` - and 0.4.x predates the check, so it cannot refuse it.
**Downgrading to 0.4.x is not supported once 0.5.0 has written to a database: do not run 0.4.x on
data 0.5.0 has written.** From 0.5.0 on, a release refuses data written by a newer one.

The check runs in `coraine-import` too: it opens the stores the way the broker does.

## Migrating from another broker

Another broker's data - entities, subscriptions, registrations and temporal history, with their ids
and system timestamps - is moved into coraine's stores with `coraine-import`, before the broker is
started on them: [Migrating a database to coraine](migration.md). Orion-LD's databases are read today.

## Administration

Load the `admin` API plugin (`--apiPlugins admin`) to get:

| Endpoint | Purpose |
|----------|---------|
| `GET /admin/health` | liveness |
| `GET /admin/version` | version, git SHA, build timestamp |
| `GET /admin/log` | current log/trace levels — `PUT`/`POST`/`PATCH`/`DELETE` change them at runtime |
| `GET /admin/tenants` | the tenants in use |
| `GET /admin/plugins` | which plugins are loaded |
| `GET /admin/metrics` | Prometheus metrics (when compiled in) |

Log and trace levels are changeable on a running broker through `/admin/log`, which
is the intended way to debug a live instance rather than restarting it with `-t`.

## Memory budget

In a container with a memory limit (Kubernetes `resources.limits.memory`, `docker run --memory`), a
process that goes over the limit is killed by the kernel - SIGKILL, exit code 137, no warning - and
with the `corDB` store everything it held goes with it. coraine keeps a budget below the limit and
turns requests away before it gets there:

| Memory used | What happens |
|---|---|
| below 90% of the budget | everything is served |
| 90% - 100% | writes that grow memory (POST, PUT, PATCH) are refused |
| over the budget | everything is refused except what frees memory or reports on the broker: DELETE, batch delete, `/version`, `/metrics`, `/admin/*` |

A refused request gets **503**, a `Retry-After: 5` header and the error type
`https://coraine.readthedocs.io/errors/MemoryBudgetExceeded`.

The budget is `--memoryLimit` (MiB), or - without it - 85% of the smallest cgroup memory limit the
process lives under (the container's, or a Kubernetes pod's above it). Outside a container, with no
limit, there is no budget and nothing is checked.

The memory measured, every 100 ms, is the process's anonymous and shared memory (`RssAnon + RssShmem`
in `/proc/self/status`), so it includes every library the broker links; a request pays one comparison
for it. File-backed pages are not counted - the executable, the libraries and the `corDB` log, which is
memory-mapped and grows to 1 GiB a segment: they are page cache the kernel writes back and reclaims
before it kills a process, and counting them would refuse writes on a persistent `corDB` for the size
of its log.

Metrics: `ngsild_memory_budget_bytes`, `ngsild_memory_used_bytes` (what is compared with the budget),
`ngsild_memory_resident_bytes` (the whole resident set, file-backed pages included),
`ngsild_requests_refused_memory_total`.

## Response size

**`--maxResponseSize <MiB>`** (0 = no budget, at most 4096) is the byte budget of an entity query: `GET /ngsi-ld/v1/entities` and `POST /ngsi-ld/v1/entityOperations/query`, on the live
tenant and on a Snapshot - and of the [temporal queries](#temporal-queries) and the
[forwarded part](#distributed-queries) of a distributed query. The store counts each entity as it fetches it and stops before the one that
would take the sum past the budget, so the memory a query takes is bounded while it is fetched, not
after it has been rendered. What is counted is the stored entity: with `mongoc` the length of its BSON
document, with `corDB` the size of its JSON rendering. `pick`, `omit`, `attrs` and the output format
do not change it.

Without the option, the budget is **1/16 of the [memory budget](#memory-budget)** - so it follows a
container's memory limit (or `--memoryLimit`): 27 MiB in a 512 MiB pod, 108 MiB in a 2 GiB one - and
a broker with no memory budget (no limit, no `--memoryLimit`) has none. A response holds more than its
bytes (the entities fetched, the rendered body, its send buffer) and several run at once; a sixteenth
leaves the memory budget room for them. `-v` at startup says which budget is in force and why.

`limit` keeps its meaning. The two are independent bounds, and whichever binds first ends the page:

| Case | Answer |
|---|---|
| the budget is reached before `limit` | 200 with a page shorter than `limit`. `Link` `rel="next"` points at `offset` + the entities returned, with the same `limit`; `NGSILD-Results-Count` (`count=true`) is the size of the whole result set |
| the first entity of the page alone is larger than the budget | 403, error type `https://uri.etsi.org/ngsi-ld/errors/TooManyResults` |
| the query needs every match at once - `orderBy` (the matches are ordered before they are paginated) - and they are larger than the budget | 403 `TooManyResults` |
| a page served from an [EntityMap](#entitymaps) | the page ends before the entity that would pass the budget, `rel="next"` points at the map position after the last one on it; 403 only when the first entity alone is larger |

The end of a result set is the page without a `rel="next"` link, not a page shorter than `limit`.

### Temporal queries

The same budget bounds `GET /ngsi-ld/v1/temporal/entities`, `POST /ngsi-ld/v1/temporal/entityOperations/query`
and `GET /ngsi-ld/v1/temporal/entities/{id}`. The store measures an entity's instances before it fetches
them: with `timescale` the text of each instance's values plus 128 bytes for its type and timestamps,
with `corDB` the stored record of each instance. The instances are taken by rank - the k-th instance
of every attribute (and datasetId) of the page, in the page's order - so a cut leaves every attribute
with the same number of instances, as a smaller `firstN` / `lastN` would.

| Case | Answer |
|---|---|
| a query: the budget is reached before `limit` | 200 with a page shorter than `limit`, each entity on it with every instance its temporal page gives it. `Link` `rel="next"` points at `offset` + the entities returned |
| one entity whose instances are larger than the budget - a retrieve, or the first entity of a query's page | 200 with the K instances per attribute that fit (K smaller than `firstN` / `lastN` / `--troeInstanceCap`). The temporal pagination `Link` (`rel="intervalafter"`, `rel="intervalbefore"` with `lastN`) points at `offsetN` + K, as for the instance cap (TS 104-176 § 6.4.7.3). On a query, that entity ends the page: `rel="next"` points at `offset` + 1 |
| not even one instance of every attribute of the entity fits | 403 `TooManyResults` |
| `format=aggregatedValues` computed by the store | each entity whole or not at all (a bucket is not an instance that can be left for the next page): a shorter page, or 403 when the first entity is larger than the budget |

`--troe ramDB` answers no temporal query, so it has nothing to bound.

### Distributed queries

What the Context Sources answer to a forwarded query counts against the same budget, in
`GET /ngsi-ld/v1/entities`, `POST /ngsi-ld/v1/entityOperations/query` and the temporal queries above.

Each source's answer is read up to the budget and no further: an answer whose body is larger is
dropped as it arrives, its connection closed, and the query is refused with 403 `TooManyResults`
naming the registration - nothing of it can be placed on a page. Lowering `limit` makes the
source's answer smaller. Over HTTP the read stops at the budget; an answer from a source this broker
serves in process, or over `cor://`, is already in memory when it is measured, and is dropped the same.

The answers that were read are then counted entity by entity, as rendered:

| Case | Answer |
|---|---|
| a paged query (`splitEntities=false` or `--noSplitEntities`: every entity wholly at one source) | the local page and each source's page are cut at the same depth - the most entities of each that fit in the budget together, and in `limit`. 200, `Link` `rel="next"` at `offset` + that depth: the sources are asked for the same `offset` and `limit` as the local store, so the next page continues every one of them where this one stopped |
| the depth is 0: the first entity of the local page and of each source's page together are larger than the budget | 403 `TooManyResults` |
| a query that needs every match at once - `orderBy`, and split entities (the default: an entity is assembled from the parts the sources hold before the page is cut) without an [EntityMap](#entitymaps) - and the local and forwarded matches together are larger than the budget | 403 `TooManyResults`. With automatic EntityMaps (the default) a split-entity query over the budget is paged through a map instead |
| one entity's history, `GET /temporal/entities/{id}`, with Context Sources | the instance arrays of every attribute - local and each source's - cut at the same depth (the K instances of each that fit together); `rel="intervalafter"` at `offsetN` + K |

In the build by default; `-DCOR_FEATURE_RESPONSE_BUDGET=OFF` leaves it out (no option, no budget).

## Pagination

A query whose answer is more than `limit` entities is paged; its `Link` response header points at the
next page (`rel="next"`) and, from the second page on, the previous one (`rel="prev"`), each a complete
request: every parameter of the query, `limit`, and where the page starts (TS 104-175 § 7.4.2.2,
TS 104-176 § 6.4.7.2).

**A local query** (no registration matches it, or `local=true`) in the default order (no `orderBy`) is
paged by **position**: `next` names the last entity of the page, `prev` the first -

```
Link: </ngsi-ld/v1/entities?type=T&q=speed%3E0&limit=20&pageAfter=1791549752292631675,urn:E20>;rel="next";type="application/json"
```

| parameter | the page |
|---|---|
| `pageAfter=<createdAt>,<id>` | the first `limit` matches after that entity, in the default order (creation time, then id) |
| `pageBefore=<createdAt>,<id>` | the last `limit` matches before it |

The value is the entity's creation time as the store holds it (an integer, nanoseconds) and its id - a
position given by the broker's own links, not to be built by a client (it may change between
versions). The store reads from the position on: MongoDB an index range on `{type, createdAt, _id}`
(or `{createdAt, _id}`) - no skip; corDB the entity's place in its list, found by its id (one hash
lookup), and walking back one lookup per entity for `pageBefore`. A page deep in the result costs what
the first page costs, where `offset` reads (MongoDB) or walks (corDB) every match before it.

What a page by position does that `offset` does not: an entity deleted, or one that no longer matches,
on an earlier page moves nothing - the next page starts right after the previous page's last entity,
with no entity skipped and none repeated (with `offset`, one entity fewer before the page shifts every
later page by one, and one entity is never seen). The position's own entity deleted: the page starts
where it was (its creation time; corDB walks its list to it then, as `offset` does). It is a live
result, not a frozen set: an entity that starts to match before the position is not on the pages after
it, and the pages after it show the entities as they are now. A frozen set is an EntityMap
(`entityMap=true`, below).

- A client's own `offset` pages as before, and the links of its pages carry `offset`.
- `orderBy`, `georel=near`, a query that Context Sources answer too (distributed), an EntityMap, a
  Snapshot (`NGSILD-Snapshot`): paged by `offset` (or the map's offset), as before.
- `pageAfter` / `pageBefore` with `offset`, with `orderBy`, with `georel=near`, with an EntityMap or a
  Snapshot, both together, or a value that is not a position: 400 `BadRequestData`. A position on a
  query that has become distributed since its first page (a registration matches it now): 400 - its
  pages start over.
- `NGSILD-Results-Count` (`count=true`) is the count of the whole query, on every page.
- The links are `next` and `prev` only - no `first` / `last` (the first page is the query without a
  position).

The names are coraine's: TS 104-175 § 7.4.2.3 leaves the links opaque ("only requires pointers to the
following and previous pages, which can be implemented in a completely opaque way"); `limit` and
`offset` are the "transparent" option, and a client using them gets them.

## EntityMaps

An EntityMap (TS 104-175 § 9.6) freezes the **set** of entities a query matches - their ids, in the
query's order, each with the sources that hold it - and a paginated query is served from it: a page
is a slice of the frozen ids, each entity fetched from where the map says it lives. It freezes the
set, not the values. Every page applies the query's filters (`type`, `q`, `scopeQ`, the GeoQuery)
again, and an entity that no longer matches is left out: `limit=20` with 3 that no longer match is a
page of 17, and the next page still starts 20 further on (see below). An entity created after the map
is not in it.

**Automatic EntityMaps.** A `GET /ngsi-ld/v1/entities` whose first page (`offset` 0) has more after it
gets a map without asking for one, as `--autoEntityMaps` says:

| `--autoEntityMaps` | |
|---|---|
| `distributed` (the default) | a distributed query - the sources are not asked for `offset` / `limit`, so without a map there is no correct second page |
| `all` | a local query too, when the store holds more than `limit` matches |
| `none` | none - a map only when a client asks for one |

A local query pages in the store ([Pagination](#pagination)), and an automatic map makes its first page
cost more: a second query for the ids of every match, and the memory of the map
([performance.md](performance.md#what-automatic-entitymaps-cost) has the numbers - a first page of
20 of a 1 % type in 300 000 entities: from 11 725 to 574 requests/s on MongoDB, from 16 372 to 66 on
corDB). What it buys is pages that are slices of the set as it was at the first page; a local query's
pages by position already skip and repeat nothing. A client that wants the frozen set for a local
query asks for it: `?entityMap=true`.

- a local query (`all`), when the store holds more than `limit` matches;
- a distributed query (`distributed`, `all`) (registrations matched, `--distributed`), when its answer is more than a page -
  more than `limit` local matches, a source that answered a whole page, or more than `limit` together.
  The broker then asks each source for its own EntityMap (`GET /ngsi-ld/v1/entityMaps` on the source)
  and records it (`linkedMaps`); the pages fetch each entity from its sources, asking a source that
  answered with a map for its entity from that map. With split entities (the default) the map holds
  candidates and the filters are applied to the entities assembled for each page.

Not for: a query of one page (nothing to paginate - no map, and the sources are asked exactly what a
query without maps asks them), a query that starts at another `offset`, `limit=0`, `POST
/entityOperations/query`, and `orderBy`: ordering by attribute values needs the values frozen, which
is a Snapshot (`NGSILD-Snapshot`), not an EntityMap.

| | Status | `NGSILD-EntityMap` response header | `Link` |
|---|---|---|---|
| a query that gets an automatic map | 200 - the answer is the result of the query; the map is a by-product of paginating it | the map's URI | `next` / `last` name the map |
| a page of an expired or unknown map, with the query's parameters | 200 - a new map is made from them | the new map's URI | name the new map |
| `?entityMap=true`, or `GET`/`POST /ngsi-ld/v1/entityMaps` | 201 Created (TS 104-176 clause 7: "in case an EntityMap has been (re)created") | the map's URI | the first page's `next` / `last` name the new map |
| a page of an existing map: `?entityMap=<id>`, or the request header `NGSILD-EntityMap: <URI>` | 200 | the map's URI | `first` / `prev` / `next` / `last` name the map |

Every link names the map - `entityMap=<id>` with `limit` and `offset` - and repeats the whole query
that created it: the selecting parameters (`type`, `q`, `scopeQ`, the GeoQuery, `attrs`, `id`, `csf`,
`local`, ...) and the ones that shape the answer (`options`, `format`, `pick`, `lang`, `count`, ...).
TS 104-175 § 9.6: "Subsequent requests referencing an Entity Map shall use the same parameters as in
the original request that created the Entity Map, except for the specification of Entity identifiers
or parameters related to pagination". So a followed link is a complete query by itself. A page that
sends a selecting parameter with a different value than the map's query, or one that query did not
have, is 400 `BadRequestData` naming the parameter; one that leaves a selecting parameter out is served
by the map's query. The request header `NGSILD-EntityMap` and `?entityMap=<id>` naming different maps
is 400. `NGSILD-Results-Count` (`count=true`) on a page of a map is the size of the frozen set.

An entity that no longer matches is left out of its page, and stays in the map. § 9.6 says such
entities "shall be removed from the Entity Map"; the pages are the same either way, except that a
removal would shift every later position, and the next page, at `offset` + `limit`, would skip as many
entities as were removed. So the positions of the frozen set never move, a page can be shorter than
`limit`, and `NGSILD-Results-Count` stays the size of the frozen set.

A map lives 5 minutes. An automatic map's lifetime slides: each page served from it gives it another
5 minutes. A page of a map that has expired, or is unknown: "a new one shall be created" (§ 9.6) - from
the request's own parameters (a link carries the whole query), and the requested page is served from
the new map, named in `NGSILD-EntityMap` (200). A request with no selector at all - nothing to make a
map from - gets 404.

**`--entityMapMemory <MiB>`** (default 64) is the memory all EntityMaps may hold together: an entity
id and its sources, about 70 bytes plus the id for an entity of this broker. When a new map does not
fit, the expired maps go, then the automatic maps used longest ago; a map a client asked for is never
evicted. An automatic map that still does not fit is not made - the query is paginated as it would be
without one (`offset` / `limit`, local and forwarded); a map the client asked for is refused with 403
`TooManyResults`, as is a source's EntityMap larger than the whole cap. `0` removes the cap and turns
automatic maps off. `ngsild_entity_map_bytes` and `ngsild_entity_map_store_size` (`/admin/metrics`)
show what the maps hold.

A page of a map fetches the page's entities of this broker in one call to the store (MongoDB: one
query, `{_id: {$in: [...]}}`; corDB: the id index), and the ones of each Context Source from it.

In the build by default; `-DCOR_FEATURE_AUTO_ENTITY_MAP=OFF` leaves the automatic maps and
`--autoEntityMaps` out (a map is then only made when a client asks for one).

## Health port

With **`--healthPort <port>`** the broker answers health probes on a port of its own, served by a
thread of its own - not by the HTTP server, so a probe never waits in its queue behind the requests.
Without the option nothing listens. It opens before the store is loaded.

| Request | 200 | 503 |
|---|---|---|
| `GET /live` | the broker makes progress | a request has been in flight for longer than `--healthStallTimeout` seconds (default 30) and no request has finished in that time. Never for memory or a database |
| `GET /ready` | the broker serves | the store is still loading; the current-state or the temporal store does not answer its ping; over the [memory budget](#memory-budget); stuck (as `/live`); stopping (from SIGTERM on) |
| `GET <anything else>` | always | — |

A connection that sends nothing (a TCP probe) is closed unanswered. Every answer has the same body,
the report:

```json
{
  "status": "ok",
  "uptime": 3605,
  "store": { "plugin": "mongoc", "loaded": true, "reachable": true, "lastPingAgoMs": 412, "pingLatencyUs": 310 },
  "troe": { "plugin": "none", "loaded": true, "reachable": true },
  "memory": { "budget": 912680550, "used": 61435904, "resident": 75988992, "level": "ok", "refused": 0 },
  "requests": { "inFlight": 2, "total": 1288211, "lastFinishedAgoMs": 0 }
}
```

| Member | |
|---|---|
| `status` | `starting` (the store loading), `ok`, `degraded` (the temporal store unreachable, or over 90% of the memory budget), `down` (the store unreachable, or stuck), `stopping` |
| `uptime` | seconds since the health port opened |
| `store`, `troe` | the current-state and the temporal plugin. `mongoc` and `timescale` are pinged once a second, on a connection of their own, with a 1 s timeout: `reachable` is an answer within the last 5 s, `lastPingAgoMs` and `pingLatencyUs` are the last ping's. `corDB` and `none` are in the broker's process: reachable once loaded |
| `memory` | the [memory budget](#memory-budget) in bytes (0: none), what counts against it, the whole resident set, `level` (`ok`, `soft` - writes refused, `hard` - everything but deletes and monitoring refused) and the requests refused for it |
| `requests` | the requests in flight, how many have finished, and how long ago the last one did (`null`: none yet) |

Nothing is measured when a probe arrives: the figures are kept current in the background, and an
answer formats them. In the build by default; `-DCOR_FEATURE_HEALTH=OFF` leaves it out.
How the probes are set up on Kubernetes: [Kubernetes](kubernetes.md#probes).

## Sanity check procedures

The steps below verify that a new installation is complete and working. They assume
the defaults - port **1026**, the `mongoc` plugin on a local MongoDB - and the
`admin` API plugin loaded (`--apiPlugins admin`). Adjust host and port to match.

### End-to-end test

Create an Entity, read it back and delete it. Each step names the answer that means
"working".

```sh
# 1. The broker answers, and says which build it is  -> 200, "coraine version" and each library's version
curl -s localhost:1026/admin/version

# 2. Create an Entity  -> 201 Created, with a Location header
curl -si localhost:1026/ngsi-ld/v1/entities -H 'Content-Type: application/json' \
     -d '{"id": "urn:ngsi-ld:SanityCheck:1", "type": "SanityCheck",
          "temperature": {"type": "Property", "value": 21.5}}'

# 3. Read it back  -> 200, the same Entity, "temperature" with value 21.5
curl -s localhost:1026/ngsi-ld/v1/entities/urn:ngsi-ld:SanityCheck:1

# 4. Delete it  -> 204 No Content; a second GET then answers 404
curl -si -X DELETE localhost:1026/ngsi-ld/v1/entities/urn:ngsi-ld:SanityCheck:1
```

With `--database corDB` the same steps apply; the Entity lives in memory instead.

### List of running processes

One process, `coraine`:

```sh
pgrep -a -x coraine              # a native installation
docker ps --filter name=coraine  # the Docker image
```

It runs in the background by default; `-fg` keeps it in the foreground (the Docker
image runs it that way).

### Network interfaces up and open

- **TCP 1026** (or the `--port` given), for NGSI-LD and the admin API:
  `ss -ltnp | grep 1026`
- **TCP 27017** to MongoDB, outgoing, when the `mongoc` plugin is used.

### Databases

With the `mongoc` plugin, the default tenant is the database named by `--dbName`
(default `mongoc`); every other tenant has a database of its own. After step 2 of the
end-to-end test the database exists:

```sh
mongosh --quiet --eval 'db.getMongo().getDBNames()'
```

`corDB` keeps its state in the broker process and needs no database - with
`--dbDir`, in that directory (`snap-N.cor`, `log-N.cor`, per tenant).

### Diagnosis

- **The log:** `/tmp/coraine.log`. Errors (`E:`) and warnings (`W:`) are always
  written there.
- **Liveness:** `GET /admin/health`. **Loaded plugins:** `GET /admin/plugins`.
- **More detail from a running broker:** raise the trace levels through
  `/admin/log` (see [Administration](#administration)) instead of restarting it with `-t`.
- **The broker does not start:** run it with `-fg`, which prints the reason
  on the terminal. A MongoDB that cannot be reached is the most common cause.

## Multi-tenancy

Tenants are selected per request with the `NGSILD-Tenant` header. With the `mongoc`
plugin each tenant is a separate database; with `corDB` each tenant is a separate
store (and, with `--dbDir`, a directory of its own). No configuration is needed to create one: a write naming an
unknown tenant creates it, while a read of a tenant that does not exist answers
**404 NonexistentTenant** rather than an empty result.
