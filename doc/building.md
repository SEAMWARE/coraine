# Building from source

Most people never need this page: the published image runs the broker without a
compiler anywhere in sight — see [Quick start](https://github.com/SEAMWARE/coraine#quick-start).
Build from source when you want to change coraine, to run it somewhere no image
suits, or to compile features out (below).

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
│   │   └── metrics/         # Prometheus via kprom
│   └── plugins/
│       ├── currentState/    # mongoc, corDB   (DB plugins)
│       ├── temporal/        # none, ramdb, timescale  (TRoE plugins)
│       ├── api/admin/       # admin API plugin
│       └── shared/          # geoMatch.c etc. shared across plugins
├── test/funcTests/          # corTest functional tests
├── doc/                     # plugin architecture, functest coverage audit
├── CMakeLists.txt           # real build (feature flags, lib wiring)
└── makefile                 # convenience wrapper (release/debug/install/test/coverage)
```

## What it links against

coraine links a constellation of sibling repos (k-libs + Cor-Libs) plus several
system libraries. The repos must sit as **siblings** under one parent (default
`~/git`), because the build references `../<lib>/lib<lib>.a`.

## Fastest path — bootstrap script

If you're starting from scratch, clone the `corLibs` umbrella and run its
`bootstrap.sh`: it clones every dependency as a sibling at its pinned version and
builds the whole lib stack. It works wherever you put it - the layout is derived
from the umbrella's own location, not from a fixed path.

```sh
git clone git@github.com:SEAMWARE/corLibs.git
./corLibs/bootstrap.sh
```

Then:

```sh
cd ~/git/coraine
make di            # debug build + install (binary + plugins → /opt/seamware, /usr/local/bin)
```

## Dependency stack

- **k-libs** (gitlab.com/kzangeli): `kbase kalloc klog khash kjson kargs ktrace kprom`
- **Cor-Libs** (github.com/SEAMWARE): `corRest corNgsild corJsonld corPlugin`
- **umbrella / test runner**: `corLibs`, `corTest`

`make` auto-rebuilds `corRest`/`corNgsild`/`corJsonld` (the broker's `libs` target);
the k-libs and `corPlugin` must already be built (the umbrella or bootstrap handles
that).

## System packages (Debian/Ubuntu)

| Need | Package |
|------|---------|
| HTTP server | `libmicrohttpd-dev` |
| TLS | `libssl-dev` |
| MQTT (notifications) | `libmosquitto-dev` |
| Geo queries | `libgeos-dev` |
| MongoDB driver (mongoc plugin) | mongo-c **v2** (`mongoc2.pc` via pkg-config) |
| TimescaleDB plugin | `libpq-dev` |
| Toolchain | `cmake build-essential` |

> Don't need Mongo? Build without it: `cmake -DCOR_FEATURE_MONGOC=OFF` and run with
> `--database corDB`. The mongo-c v2 driver is the most common build snag.

## Make targets

| Target | Effect |
|--------|--------|
| `make` / `make release` | Release build (`BUILD_RELEASE/`) |
| `make debug` | Debug build (`BUILD_DEBUG/`) |
| `make i` / `make di` | release/debug **+ install** |
| `make ci` / `make cdi` | clean + the above |
| `make install` | copy broker + plugins → `/usr/local/bin`, `/opt/seamware/plugins`, `/opt/seamware/etc` |
| `make clean` | remove build trees |
| `make test` | run the functional test suite (`corTest`) |
| `make coverage` | coverage report per DB (`coverage-<db>/index.html`) |
| `make coverage-etsi` | full ETSI TP suite coverage (`coverage-etsi/index.html`) |

Install writes to `/opt/seamware/...` and `/usr/local/bin` — run with appropriate
permissions or pre-create the dirs.



## Compiling out what you don't need

`CMakeLists.txt` exposes `COR_FEATURE_*` options — subscriptions, registrations,
geoq, scopes, datasetId, multi-type, context download/hosting, tenants, mongoc,
admin API, metrics, ICU collation, geo-dispatch on
location/observationSpace/operationSpace. All default ON except the
observation/operation-space dispatch; toggle with `cmake -DCOR_FEATURE_X=OFF`.

The intent is a broker you can shrink to exactly the NGSI-LD you actually deploy —
no subscription engine on a read-only edge node, no geo, no tenants, no Mongo.

Where it stands today, honestly: **the flags are declared, the work behind them
has barely started.** What works is selection at the build-tree level —
`-DCOR_FEATURE_MONGOC=OFF` builds a Mongo-free tree (drop `libmongoc` from the
build host, run `--database corDB`). Everything else is still a promise: the
per-feature `#ifdef`s inside the C are next to nonexistent, so switching off a
core feature leaves its symbols referenced from code that still compiles, and
the link fails. The same holds for the optional runtime deps — MQTT
notifications, for instance, are ~2 KB of broker code against a `libmosquitto`
that every build links and every process maps, whether or not a single MQTT
notification is ever sent. Shrink-to-fit is a goal with a flag table, not a
feature you can use yet.

## Next

Once it builds, [Testing](testing.md) covers running the suite and measuring
coverage.
