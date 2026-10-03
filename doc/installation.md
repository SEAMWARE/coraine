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
| `--maxRequestSize` / `-mrs` | 2 | max request body, MiB (0 = no cap, § 6.3.2) |
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
every 100 ms, with a snapshot as the log grows. A clean stop (SIGTERM) loses nothing;
`kill -9` or a power cut loses at most the last unsynced interval.

| Option | Default | Meaning |
|--------|---------|---------|
| `--dbDir` | none: in RAM only | the directory - one subdirectory per tenant |
| `--dbSync` | `interval` | `interval`: synced every `--dbSyncInterval` ms; `request`: a write answers once its record is on the disk; `none`: written, never synced |
| `--dbSyncInterval` | 100 | ms between two syncs |
| `--dbSnapshotEvery` | 64 | MiB of log after which a tenant is snapshotted (and at least as much as its last snapshot) |

What it costs: [Performance](performance.md), "corDB on disk". How it works:
[corDB's persistence](https://github.com/SEAMWARE/corDB/blob/main/doc/persistence.md).

### ramDB - corDB in RAM only

**`--database ramDB`** is corDB built without the disk: the same store and the same API, nothing
written anywhere. It is for a deployment that wants the fastest store and can afford to lose it -
pub/sub, where a broker passes notifications on and what it holds is only ever the latest value, or
a cache in front of another system. **A restart starts empty**, by design, and nothing warns about it
at runtime: choose it knowing that.

- No disk options: `--dbDir` is an unknown option with ramDB, not one quietly ignored.
- No history: `--troe corDB` is refused (corDB keeps history inside its own store, on disk). The
  other TRoE choices work: `--troe none` (the default), or `--troe timescale` for history in
  PostgreSQL.
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
| `--high-precision` | `CORAINE_HIGH_PRECISION` |
| … | … |

`coraine -U` (extended usage) prints the environment-variable name of every option
alongside its type and default, so the list never has to be maintained by hand.
A command-line argument overrides the environment variable.

One variable is read outside that mechanism, by the plugin loader itself before the
arguments are parsed:

| Variable | Default | Meaning |
|----------|---------|---------|
| `SEAMWARE_PLUGIN_DIR` | `/opt/seamware/plugins` | base directory plugin short names resolve against |

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

| Resident memory | What happens |
|---|---|
| below 90% of the budget | everything is served |
| 90% - 100% | writes that grow memory (POST, PUT, PATCH) are refused |
| over the budget | everything is refused except what frees memory or reports on the broker: DELETE, batch delete, `/version`, `/metrics`, `/admin/*` |

A refused request gets **503**, a `Retry-After: 5` header and the error type
`https://coraine.readthedocs.io/errors/MemoryBudgetExceeded`.

The budget is `--memoryLimit` (MiB), or - without it - 85% of the smallest cgroup memory limit the
process lives under (the container's, or a Kubernetes pod's above it). Outside a container, with no
limit, there is no budget and nothing is checked. The memory measured is the process's resident set,
every 100 ms, so it includes every library the broker links; a request pays one comparison for it.

Metrics: `ngsild_memory_budget_bytes`, `ngsild_memory_resident_bytes`,
`ngsild_requests_refused_memory_total`.

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
