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

Where it stands today, honestly: **three of them work, the rest are declared and
not implemented.**

`-DCOR_FEATURE_MONGOC=OFF` builds a Mongo-free tree (drop `libmongoc` from the
build host, run `--database corDB`).

`-DCOR_FEATURE_REGISTRATIONS=OFF` drops the Context Source Registration,
registration-subscription and EntityMap service routines, the forwarding
library, and both DB plugins' registration code — about 24 kB of `.text`.

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
client to fix its URL; this says the deployment declined the capability and the
client's move is a different deployment. A plugin's own routes answer the same
way — the admin plugin's `subStats/flush` is compiled out with subscriptions and
returns the same 501. The type URI is ours rather than an
ETSI one because TS 104-176 § 6.3.2 registers no error type for a build-time
omission — the one 501 in that table, `NoMultiTenantSupport`, is reserved for a
single capability. See spec-doubt #124.

Ask a binary what it carries, without starting it:

```console
$ coraine --version
coraine 0.4.0
features: SUBSCRIPTIONS=1 REGISTRATIONS=0 GEOQ=1 ...
```

and ask a running one for the whole picture with **`GET /build`** — the features
compiled in, the plugins this build produced against the ones actually loaded,
and the run-time settings that change what a client gets:

```json
{
  "product": "coraine",
  "version": "0.4.0",
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

### ⚠️ Building a stripped-down broker is at your own risk

Every `COR_FEATURE_*` option can be set to `OFF` and most of them will build.
That is not the same as most of them working. Below is what each one actually
does today, measured by building the broker fourteen times with one feature off
at a time and comparing the result against a full build.

**These two remove code from the broker and are covered by the test suite:**

| flag | `.text` removed | endpoints affected |
|---|---|---|
| `COR_FEATURE_REGISTRATIONS=OFF` | 23,936 bytes | 15 routes answer 501 |
| `COR_FEATURE_SUBSCRIPTIONS=OFF` | 18,880 bytes | 7 routes answer 501 |

**These two leave the broker unchanged and drop a plugin**, which is the whole
of their effect — the broker is a plugin loader and it simply has one fewer to
load:

| flag | effect |
|---|---|
| `COR_FEATURE_MONGOC=OFF` | `mongoc.so` is not built; run `--database corDB` |
| `COR_FEATURE_ADMIN_API=OFF` | `admin.so` is not built |

**⚠️ These nine are declared and do nothing at all:**

`GEOQ`, `SCOPES`, `DATASETID`, `MULTI_TYPE`, `CONTEXT_DL`, `TENANTS`,
`LOCATION`, `OBSERVATION_SPACE`, `OPERATION_SPACE`

The option exists, CMake emits the `-D`, **no source reads it**, and the
compiled binary is byte-for-byte the same as a full build. `-DCOR_FEATURE_TENANTS=OFF`
produces a broker that builds, starts, reports `"TENANTS": false` on `GET /build`
— and serves tenants exactly as before. This is the failure mode to watch for,
because nothing about it looks like a failure. If you are switching one of these
off to remove a capability, you have not removed it.

**⚠️ These three do not build:**

`CONTEXT_HOSTING`, `METRICS`, `ICU_COLLATION`

Their `CMakeLists.txt` drops the source files, and code that survives still
references the symbols, so the link fails with `undefined reference to
'getJsonldContexts'`, `'metricsPreService'` and friends. A failed link is the
honest outcome of the three — it is the nine above that will mislead you.

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
cmake -DCOR_HTTP_SERVER=builtin    # the server in corRest - not wired up yet
```

Not a `COR_FEATURE_*` boolean, because those answer "is this capability in the
build" and the HTTP server is always in it — what varies is which one. The value
reaches CMake **and** corRest's own make (the HTTP server lives in corRest,
which builds with plain make and knows nothing of CMake options), and it decides
whether `libmicrohttpd` is on the link line at all, so `ldd coraine` is the
check that it took. A running broker reports it as `build.httpServer` on
`GET /build`.

`builtin` is refused at configure time today, with that reason: the switch is
plumbed end to end, the backend it selects is not written yet, and a switch that
produced an unlinkable binary would be worse than one that says what it is
waiting for.

Why bother: `libmicrohttpd` is ~180 kB of mapped code, about 21% on top of the
broker's own, for a library coraine uses 32 of the 81 exported symbols of — and
it is the last third-party runtime dependency besides libc and OpenSSL once the
optional features are compiled out. It is also a tarball fetched from
ftp.gnu.org and built from source in the Dockerfile.

### The optional runtime dependencies

The same holds for the optional runtime deps — MQTT notifications, for
instance, are ~2 KB of broker code against a `libmosquitto` that every build
links and every process maps, whether or not a single MQTT notification is ever
sent.

## Next

Once it builds, [Testing](testing.md) covers running the suite and measuring
coverage.
