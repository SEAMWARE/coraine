# swBroker

A lightweight **NGSI-LD Context Broker** written in C, targeting **ETSI GS CIM 009
v1.9.1**. swBroker is small, fast, and — most importantly — **plugin-driven**:
storage backends, temporal history, and extra API surfaces are all `.so` plugins
loaded at startup. The core broker speaks NGSI-LD; the plugins decide *where data
lives* and *what extra endpoints exist*.

- **Product version:** 0.3
- **Spec target:** ETSI GS CIM 009 v1.9.1 (NGSI-LD)
- **Language / build:** C, CMake (wrapped by a convenience `makefile`)
- **License / © :** Seamware

For a feature-by-feature breakdown of what's implemented, see
[`doc/implementation-status.md`](doc/implementation-status.md).

---

## Table of contents

- [Plugin architecture](#plugin-architecture) ← **start here**
- [Building](#building)
- [Running](#running)
- [Project layout](#project-layout)
- [Testing](#testing)

---

## Plugin architecture

> **This is the heart of swBroker.** The broker binary contains the NGSI-LD
> protocol logic, the REST layer, the JSON-LD engine and the subscription
> matcher. It contains **no storage code and no temporal code**. Those — plus any
> non-NGSI-LD admin/ops endpoints — are dynamically loaded shared objects. You
> choose them at startup; you can write your own without touching the core.

### The three plugin categories

| Category | Purpose | Loaded via | Resolves to | Register symbol | Driver struct |
|----------|---------|------------|-------------|-----------------|---------------|
| **Current-state DB** | Where entities/subscriptions/registrations live | `--database` / `-db` | `<base>/db/currentState/<name>.so` | `dbRegister` | `DbDriver` (`db`) |
| **Temporal (TRoE)** | History of entity evolution | `--troe` / `-troe` | `<base>/troe/temporal/<name>.so` | `troeRegister` | `TroeDriver` (`troe`) |
| **API** | Extra HTTP endpoints (ops/admin/etc.) | `--apiPlugins` / `-api` | `<base>/api/<name>.so` | `apiRegister` | `ApiPlugin[]` |

Exactly **one** DB plugin and **one** TRoE plugin are active at a time; **any
number** of API plugins (comma-separated, up to `API_PLUGINS_MAX = 16`).

### Where plugins are loaded from

The base directory defaults to **`/opt/seamware/plugins`** and is overridable by
the **`SEAMWARE_PLUGIN_DIR`** environment variable
(`swPluginSetBaseDir("/opt/seamware/plugins", "SEAMWARE_PLUGIN_DIR")` in
`swBroker.c`). `make install` copies the bundled plugins into this tree:

```
/opt/seamware/plugins/
├── db/currentState/
│   ├── mongoc.so          # MongoDB-backed store
│   └── swRamDB.so         # in-memory store
├── troe/temporal/
│   ├── none.so            # no-op (temporal disabled)
│   ├── ramdb.so           # in-memory history (dev/test)
│   └── timescale.so       # TimescaleDB/Postgres history
└── api/
    └── admin.so           # health/version/log/tenants/plugins
```

A plugin can also be given as a **full path** (any argument containing a `/`),
which bypasses base-dir resolution — handy for pointing at a freshly-built `.so`
in a build tree without installing:

```sh
swBroker --database $PWD/BUILD_DEBUG/src/plugins/currentState/swRamDB/swRamDB.so
```

### How loading works (the mechanism)

`src/lib/plugin/pluginLoader.c` does, per plugin:

1. `swPluginResolve(base, category, subcategory, name, path, …)` → builds the `.so`
   path (skipped when `name` already looks like a path).
2. `swPluginOpen(path, "<symbol>", …)` → `dlopen` + `dlsym` for the register symbol
   (`dbRegister` / `troeRegister` / `apiRegister`). Handles are tracked for
   `swPluginCloseAll()` at shutdown.
3. The register function is called with a zeroed driver struct, which it fills with
   its function pointers.

Plugins do **not** statically link the NGSI-LD/k-lib symbols — the broker is linked
`rdynamic`, so a plugin `.so` resolves `kjson`, `swNgsild`, etc. from the running
broker at `dlopen` time. Keep that in mind: a plugin must be built against the
**same** lib headers as the broker it will be loaded into.

### Plugin-contributed CLI args

A plugin can publish its own command-line options. It sets `driverP->args`
(a `KArg*` array) in its register function; the broker **peeks** at
`--database`/`--troe`/`--apiPlugins` *before* the main parse, loads the plugins,
then splices each plugin's `args` into the global arg table so they show up in
`--help` and parse normally. This is why `swBroker --help` shows different options
depending on which DB/TRoE plugin you selected.

### NULL-allowed methods → graceful 501

Driver structs are big, and not every plugin implements every operation. The
convention: a **NULL function pointer means "unsupported"**, and the service
routine returns **501 Not Implemented** (or treats it as a no-op where the spec
allows). Examples called out in the headers: `subscriptionStatsFlush`,
`snapshot*`, `tenantDrop`, and the whole context-persistence quartet
(`contextSave/Delete/List/Get`) are NULL on `swRamDB`. This is how the in-memory
driver legitimately ships without persistence.

### The driver interfaces

The contracts a plugin fills are fully documented (with per-function semantics) in
the headers — read these before writing a plugin:

- **`src/lib/db/DbDriver.h`** — current-state DB. Entity CRUD + bulk ops,
  subscriptions, registrations, snapshots, discovery (`typeList`/`attrList`),
  tenant setup, geo-match callbacks, and optional JSON-LD context persistence.
  Error codes: `DB_OK`, `DB_NOT_FOUND`, `DB_ALREADY_EXISTS`, `DB_INVALID_GEOMETRY`,
  `DB_BAD_INPUT`, `DB_ERR`.
- **`src/lib/troe/TroeDriver.h`** — temporal. The broker queues `TroeEvent`s
  during a request and drains them *after* the response (per-event or bulk
  `eventList`); read paths return `EntityTemporal` trees. Error codes: `TROE_OK`,
  `TROE_NOT_FOUND`, `TROE_UPDATED`, `TROE_ERR`.
- **`src/lib/plugin/ApiPlugin.h`** — extra endpoints. A flat
  `SwRestServiceSimplified[]` (verb + path + handler), optional URL `params`,
  optional `args`, and `init`/`close`/`versionInfo` hooks.

### Bundled plugins

| Plugin | Category | Notes |
|--------|----------|-------|
| **mongoc** | DB | MongoDB via `libmongoc` v2; `$geoNear` aggregation, persistence, context hosting, per-tenant DBs. The default (`--database mongoc`). Needs the mongo-c **v2** driver at build time. |
| **swRamDB** | DB | In-memory; GEOS geo-filtering, per-tenant isolation. No persistence by design. Ideal for tests and demos. |
| **none** | TRoE | No-op. Temporal disabled. The default (`--troe none`). |
| **ramdb** | TRoE | In-memory history; exposes a dev `dumpInfo`. Dev/test. |
| **timescale** | TRoE | TimescaleDB/Postgres-backed history (hypertables). |
| **admin** | API | `/admin/health`, `/admin/version`, `/admin/log` (GET/PUT/POST/PATCH/DELETE for verbose/debug/traceLevels), `/admin/tenants`, `/admin/plugins`. |

### Writing a new plugin (sketch)

A DB plugin is one `.so` exporting `void dbRegister(DbDriver*)`. Minimal shape,
mirroring `src/plugins/currentState/swRamDB/ramdbRegister.c`:

```c
#include "db/DbDriver.h"

void dbRegister(DbDriver* driverP)
{
  driverP->alias          = "myStore";
  driverP->version        = "0.1.0";
  driverP->args           = myArgV;          // or NULL
  driverP->init           = myInit;          // post-arg-parse init
  driverP->close          = myClose;
  driverP->entityCreate   = myEntityCreate;
  driverP->entityRetrieve = myEntityRetrieve;
  driverP->entityQuery    = myEntityQuery;
  driverP->entityDelete   = myEntityDelete;
  // … fill what you support; leave the rest NULL (→ 501)
  driverP->tenantSetup    = myTenantSetup;
}
```

Build it as a `SHARED` library that drops `myStore.so` into
`<base>/db/currentState/`, then run `swBroker --database myStore`. The existing
plugin `CMakeLists.txt` files (e.g.
`src/plugins/currentState/mongoc/CMakeLists.txt`) are the template — note they
**don't** link the broker's libs (resolved at runtime), only their own backend
deps (`mongoc2`, `geos_c`, …). API and TRoE plugins follow the same pattern with
`apiRegister`/`troeRegister`.

---

## Building

swBroker links a constellation of sibling repos (k-libs + sw-libs) plus several
system libraries. The repos must sit as **siblings** under one parent (default
`~/git`), because the build references `../<lib>/lib<lib>.a`.

### Fastest path — bootstrap script

If you're starting from scratch, the `bootstrap-swlibs.sh` script (kept next to
the repos under `~/git`) clones every dependency at pinned versions and builds the
whole lib stack via the `swLibs` umbrella. Then:

```sh
cd ~/git/swBroker
make di            # debug build + install (binary + plugins → /opt/seamware, /usr/local/bin)
```

### Dependency stack

- **k-libs** (gitlab.com/kzangeli): `kbase kalloc klog khash kjson kargs ktrace kprom`
- **sw-libs** (github.com/kzangeli): `swRest swNgsild swJsonld swPlugin`
- **umbrella / test runner**: `swLibs`, `swTest`

`make` auto-rebuilds `swRest`/`swNgsild`/`swJsonld` (the broker's `libs` target);
the k-libs and `swPlugin` must already be built (the umbrella or bootstrap handles
that).

### System packages (Debian/Ubuntu)

| Need | Package |
|------|---------|
| HTTP server | `libmicrohttpd-dev` |
| TLS | `libssl-dev` |
| MQTT (notifications) | `libmosquitto-dev` |
| Geo queries | `libgeos-dev` |
| MongoDB driver (mongoc plugin) | mongo-c **v2** (`mongoc2.pc` via pkg-config) |
| TimescaleDB plugin | `libpq-dev` |
| Toolchain | `cmake build-essential` |

> Don't need Mongo? Build without it: `cmake -DSW_FEATURE_MONGOC=OFF` and run with
> `--database swRamDB`. The mongo-c v2 driver is the most common build snag.

### Make targets

| Target | Effect |
|--------|--------|
| `make` / `make release` | Release build (`BUILD_RELEASE/`) |
| `make debug` | Debug build (`BUILD_DEBUG/`) |
| `make i` / `make di` | release/debug **+ install** |
| `make ci` / `make cdi` | clean + the above |
| `make install` | copy broker + plugins → `/usr/local/bin`, `/opt/seamware/plugins`, `/opt/seamware/etc` |
| `make clean` | remove build trees |
| `make test` | run the functional test suite (`swTest`) |
| `make coverage` | unit coverage report (`coverage/index.html`) |
| `make coverage-etsi` | full ETSI TP suite coverage (`coverage-etsi/index.html`) |

Install writes to `/opt/seamware/...` and `/usr/local/bin` — run with appropriate
permissions or pre-create the dirs.

### Feature flags

`CMakeLists.txt` exposes `SW_FEATURE_*` options (subscriptions, registrations,
geoq, scopes, datasetId, multi-type, context download/hosting, tenants, mongoc,
admin API, metrics, geo-dispatch on location/observationSpace/operationSpace). All
default ON except the observation/operation-space dispatch. Toggle with
`cmake -DSW_FEATURE_X=OFF`. Note: most flags currently gate *which sources compile
in*; the corresponding `#ifdef`s in the C are still being filled in, so turning one
off may drop symbols referenced elsewhere — treat them as scaffolding for now.

---

## Running

Default listen port is **1026**. Plugins default to `mongoc` (DB) + `none` (TRoE),
no API plugins.

```sh
# In-memory, foreground, pretty JSON, admin API on — zero external services:
swBroker --database swRamDB --troe none --apiPlugins admin --foreground -pp 2

# Default (Mongo) on a custom port:
swBroker --port 1027 --database mongoc

# Full plugin help (includes the selected plugins' own args):
swBroker --apiPlugins admin --database mongoc --usage
```

Selected common options (`--usage` for the full list):

| Option | Default | Meaning |
|--------|---------|---------|
| `--port` / `-p` | 1026 | TCP listen port |
| `--database` / `-db` | `mongoc` | DB plugin (short name or path) |
| `--troe` / `-troe` | `none` | TRoE plugin (`none` disables history) |
| `--apiPlugins` / `-api` | — | comma-separated API plugins |
| `--foreground` / `-fg` | off | don't daemonize |
| `--pretty-print` / `-pp` | 0 | JSON indent (0 = compact) |
| `--localOnly` / `-local` | off | disable distributed operations |
| `--defaultUserContext` / `-duc` | — | default `@context` URL |
| `--corsOrigin` | — | enable CORS (`__ALL` for any origin) |
| `--maxRequestSize` / `-mrs` | 2 | max body MiB (§ 6.3.2; 0 = no cap) |

---

## Project layout

```
swBroker/
├── src/
│   ├── app/swBroker/        # main(), arg table, plugin wiring, NGSI-LD service map
│   ├── lib/
│   │   ├── plugin/          # pluginLoader.c + ApiPlugin.h  (the loader)
│   │   ├── db/              # DbDriver.h  (current-state plugin contract) + tenant
│   │   ├── troe/            # TroeDriver.h (temporal plugin contract) + dispatch
│   │   ├── serviceRoutines/ # NGSI-LD endpoint handlers
│   │   ├── linkedEntities/  # join / linked-entity support
│   │   ├── forwarding/      # distributed-ops (CSR) forwarding
│   │   └── metrics/         # Prometheus via kprom
│   └── plugins/
│       ├── currentState/    # mongoc, swRamDB   (DB plugins)
│       ├── temporal/        # none, ramdb, timescale  (TRoE plugins)
│       ├── api/admin/       # admin API plugin
│       └── shared/          # geoMatch.c etc. shared across plugins
├── test/funcTests/          # swTest functional tests
├── doc/                     # implementation status, feature overview, port triage
├── CMakeLists.txt           # real build (feature flags, lib wiring)
└── makefile                 # convenience wrapper (release/debug/install/test/coverage)
```

---

## Testing

Functional tests run through `swTest` (installed by the `swLibs` umbrella into
`~/git/swLibs/bin/swTest`):

```sh
make test                    # whole suite against a swRamDB broker
```

Tests live under `test/funcTests/`. Coverage:

```sh
make coverage                # unit/functional coverage  → coverage/index.html
make coverage-etsi           # ETSI TP suite coverage     → coverage-etsi/index.html
```

The ETSI target instruments the broker **and** the NGSI-LD libs (whole-archived,
so they flush through the broker's gcov runtime) **and** the mongoc/timescale
plugin `.so`s.
