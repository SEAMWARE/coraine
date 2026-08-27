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
| MQTT notifications | `libmosquitto-dev` |
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

[Building from source](building.md) is the full account — the source layout, every
system package, all the make targets, and how to compile features out. This page
covers the common case only.

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
| `--high-availability` / `-ha` | — | keep the caches in step with the other instances (`mongo` = change streams; needs the `mongoc` DB **and** a replica set) |
| `--version` / `-V` | — | print the version and exit |
| `--traceLevels` / `-t` | — | trace levels for debugging |

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

## Multi-tenancy

Tenants are selected per request with the `NGSILD-Tenant` header. With the `mongoc`
plugin each tenant is a separate database; with `corDB` each tenant is a separate
in-memory store. No configuration is needed to create one: a write naming an
unknown tenant creates it, while a read of a tenant that does not exist answers
**404 NonexistentTenant** rather than an empty result.
