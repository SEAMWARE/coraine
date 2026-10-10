# Building from source

Most people never need this page: the published image runs the broker without a
compiler anywhere in sight — see [Quick start](https://github.com/SEAMWARE/coraine#quick-start).
Build from source when you want to change coraine, to run it somewhere no image
suits, or to compile features out.

This page is how to build and the make targets. The source layout, every build
option - the features that compile out and the state of each, the HTTP server -
are in [the details](building-details.md). How it came about:
[history](history/building.md).

## Fastest path — bootstrap script

If you're starting from scratch, clone the `corLibs` umbrella and run its
`bootstrap.sh`: it clones every dependency as a sibling and
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

## What it links against

coraine links a constellation of sibling repos (the Cor-Libs) plus several
system libraries. The repos must sit as **siblings** under one parent (default
`~/git`), because the build references `../<lib>/lib<lib>.a`.

## Dependency stack

- **Cor-Libs** (github.com/SEAMWARE): `corBase corLog corAlloc corArgs corHash corTree corJson corProm corRest corNgsild corJsonld corPlugin`
- **umbrella / test runner**: `corLibs`, `corTest`

`make` auto-rebuilds `corRest`/`corNgsild`/`corJsonld` (the broker's `libs` target);
the other libs must already be built (the umbrella or bootstrap handles
that).

## System packages (Debian/Ubuntu)

| Need | Package |
|------|---------|
| HTTP server | `libmicrohttpd-dev` |
| TLS | `libssl-dev` |
| MQTT bridge plugin, `mqtt.so` ([corMqttBridge](https://github.com/SEAMWARE/corMqttBridge)) | `libmosquitto-dev` |
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
| `make pgo` | Profile-guided release build (`BUILD_PGO/`): instrumented, trained on `test/perf/pgoTrain.sh`, rebuilt - ~6 min, needs `wrk`; +1-10 % per core ([performance](performance.md)) |
| `make i` / `make di` | release/debug **+ install** |
| `make ci` / `make cdi` | clean + the above |
| `make install` | copy broker + plugins → `/usr/local/bin`, `/opt/seamware/plugins`, `/opt/seamware/etc` |
| `make clean` | remove build trees |
| `make test` | run the functional test suite (`corTest`) |
| `make coverage` | coverage report per DB (`coverage-<db>/index.html`) |
| `make coverage-etsi` | full ETSI TP suite coverage (`coverage-etsi/index.html`) |

Install writes to `/opt/seamware/...` and `/usr/local/bin` — run with appropriate
permissions or pre-create the dirs.

Install with brokers running: each file is copied beside the installed one and renamed
over it, never written in place. A running broker keeps the files it started with and
loads the new ones at its next start. (A copy *onto* a plugin a broker has loaded
changes that broker's code under it - it dies of SIGSEGV within seconds.)
`test/install/installUnderRunningBroker.sh` checks it.

## Build options

| | |
|---|---|
| `cmake -DCOR_FEATURE_<X>=OFF` | compile a feature out - not every flag does what it says; [the details](building-details.md#compiling-out-what-you-dont-need) list the state of each |
| `cmake -DCOR_HTTP_SERVER=mhd\|builtin` | the HTTP server: libmicrohttpd (the default) or corHttp, with no external HTTP dependency ([details](building-details.md#choosing-the-http-server)) |
| `make di CMAKE_FEATURES=... BUILD_DEBUG=<dir>` | a reduced tree in a directory of its own, beside the ordinary one |

`coraine --version` and `GET /build` say what a binary carries; check them after any
reduced build rather than assuming the flag took.

## Next

Once it builds, [Testing](testing.md) covers running the suite and measuring
coverage.
