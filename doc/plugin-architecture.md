# coraine — Plugin architecture

> **This is the heart of coraine.** The broker binary contains the NGSI-LD
> protocol logic, the REST layer, the JSON-LD engine and the subscription
> matcher. It contains **no storage code and no temporal code**. Those — plus any
> non-NGSI-LD admin/ops endpoints — are dynamically loaded shared objects. You
> choose them at startup; you can write your own without touching the core.

## The four plugin categories

There are **four** kinds of plugin:

- **Current-state DB** — where entities, subscriptions and registrations live.
  Loaded via `--database` / `-db`; resolves to `<base>/db/currentState/<name>.so`;
  register symbol `dbRegister`; fills the `DbDriver` struct (`db`). **One active at
  a time.** Bundled: `mongoc` (default), `corDB`.

- **History DB** — the temporal evolution of entities (TRoE — Temporal
  Representation of Entities). Loaded via `--troe` / `-troe`; resolves to
  `<base>/troe/temporal/<name>.so`; register symbol `troeRegister`; fills the
  `TroeDriver` struct (`troe`). **One active at a time** (`none` disables history).
  Bundled: `none` (default), `ramdb`, `timescale`.

- **API services** — extra HTTP endpoints beyond the NGSI-LD core (ops, admin,
  health, …). Loaded via `--apiPlugins` / `-api`; resolves to `<base>/api/<name>.so`;
  register symbol `apiRegister`; fills an `ApiPlugin` entry. **Any number** active
  (comma-separated, up to `API_PLUGINS_MAX = 16`). Bundled: `admin`.

- **Communication protocols** — the transport over which the broker speaks to
  clients and to other brokers. The default is **REST/HTTP**, which is what ships
  today. A pluggable transport layer that lets an **ad-hoc binary protocol** be
  swapped in alongside (or instead of) REST is **planned, not yet implemented** —
  the architecture is designed around it, but there is no `protocol` register
  symbol or driver struct yet.

So today three of the four are live; the communication-protocol category is the
next plugin axis to land.

## Where plugins are loaded from

The base directory defaults to **`/opt/seamware/plugins`** and is overridable by
the **`SEAMWARE_PLUGIN_DIR`** environment variable
(`corPluginSetBaseDir("/opt/seamware/plugins", "SEAMWARE_PLUGIN_DIR")` in
`coraine.c`). `make install` copies the bundled plugins into this tree:

```
/opt/seamware/plugins/
├── db/currentState/
│   ├── mongoc.so          # MongoDB-backed store
│   └── corDB.so         # in-memory store
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
coraine --database $PWD/BUILD_DEBUG/src/plugins/currentState/corDB/corDB.so
```

## How loading works (the mechanism)

`src/lib/plugin/pluginLoader.c` does, per plugin:

1. `corPluginResolve(base, category, subcategory, name, path, …)` → builds the `.so`
   path (skipped when `name` already looks like a path).
2. `corPluginOpen(path, "<symbol>", …)` → `dlopen` + `dlsym` for the register symbol
   (`dbRegister` / `troeRegister` / `apiRegister`). Handles are tracked for
   `corPluginCloseAll()` at shutdown.
3. The register function is called with a zeroed driver struct, which it fills with
   its function pointers.

Plugins do **not** statically link the NGSI-LD/k-lib symbols — the broker is linked
`rdynamic`, so a plugin `.so` resolves `kjson`, `corNgsild`, etc. from the running
broker at `dlopen` time. Keep that in mind: a plugin must be built against the
**same** lib headers as the broker it will be loaded into.

## Plugin-contributed CLI args

A plugin can publish its own command-line options. It sets `driverP->args`
(a `KArg*` array) in its register function; the broker **peeks** at
`--database`/`--troe`/`--apiPlugins` *before* the main parse, loads the plugins,
then splices each plugin's `args` into the global arg table so they show up in
`--help` and parse normally. This is why `coraine --help` shows different options
depending on which DB/TRoE plugin you selected.

## NULL-allowed methods → graceful 501

Driver structs are big, and not every plugin implements every operation. The
convention: a **NULL function pointer means "unsupported"**, and the service
routine returns **501 Not Implemented** (or treats it as a no-op where the spec
allows). Examples called out in the headers: `subscriptionStatsFlush`,
`snapshot*`, `tenantDrop`, and the whole context-persistence quartet
(`contextSave/Delete/List/Get`) are NULL on `corDB`. This is how the in-memory
driver legitimately ships without persistence.

## The driver interfaces

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
  `CorRestServiceSimplified[]` (verb + path + handler), optional URL `params`,
  optional `args`, and `init`/`close`/`versionInfo` hooks.

## Bundled plugins

| Plugin | Category | Notes |
|--------|----------|-------|
| **mongoc** | DB | MongoDB via `libmongoc` v2; `$geoNear` aggregation, persistence, context hosting, per-tenant DBs. The default (`--database mongoc`). Needs the mongo-c **v2** driver at build time. |
| **corDB** | DB | In-memory; GEOS geo-filtering, per-tenant isolation. No persistence by design. Ideal for tests and demos. |
| **none** | TRoE | No-op. Temporal disabled. The default (`--troe none`). |
| **ramdb** | TRoE | In-memory history; exposes a dev `dumpInfo`. Dev/test. |
| **timescale** | TRoE | TimescaleDB/Postgres-backed history (hypertables). |
| **admin** | API | `/admin/health`, `/admin/version`, `/admin/log` (GET/PUT/POST/PATCH/DELETE for verbose/debug/traceLevels), `/admin/tenants`, `/admin/plugins`. |

## Writing a new plugin (sketch)

A DB plugin is one `.so` exporting `void dbRegister(DbDriver*)`. Minimal shape,
mirroring `src/plugins/currentState/corDB/corDbRegister.c`:

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
`<base>/db/currentState/`, then run `coraine --database myStore`. The existing
plugin `CMakeLists.txt` files (e.g.
`src/plugins/currentState/mongoc/CMakeLists.txt`) are the template — note they
**don't** link the broker's libs (resolved at runtime), only their own backend
deps (`mongoc2`, `geos_c`, …). API and TRoE plugins follow the same pattern with
`apiRegister`/`troeRegister`.

---

Back to the [README](../README.md).
