# coraine

[![FIWARE Core Context Management](https://fiware.github.io/catalogue/badges/chapters/core.svg)](https://www.fiware.org/developers/catalogue/)
[![License badge](https://img.shields.io/github/license/SEAMWARE/coraine.svg)](https://opensource.org/licenses/Apache-2.0)
[![Container badge](https://img.shields.io/badge/quay.io-seamware%2Fcoraine-grey?logo=red%20hat&labelColor=EE0000)](https://quay.io/repository/seamware/coraine)
[![Support badge](https://img.shields.io/badge/support-github%20issues-orange.svg?logo=github)](https://github.com/SEAMWARE/coraine/issues)
[![NGSI-LD badge](https://img.shields.io/badge/NGSI-LD-red.svg)](https://www.etsi.org/committee/cim)
<br/>
[![Documentation badge](https://img.shields.io/readthedocs/coraine.svg)](https://coraine.readthedocs.io/en/latest/?badge=latest)
![Status](https://fiware.github.io/catalogue/badges/statuses/status-incubating.svg)

A lightweight **NGSI-LD Context Broker** written in C, **fully implementing ETSI GS
CIM 009 v1.9.1** and passing the official **ETSI NGSI-LD conformance test suite
with a 100% success rate**.<sup>\*</sup>

coraine is small, fast, and — most importantly — **plugin-driven**. Loaded at
startup as shared libraries:

- **storage backends** — where the current state lives
- **temporal history** — the temporal evolution of entities (TRoE)
- **extra API surfaces** — ops/admin endpoints beyond NGSI-LD
- **the wire protocol itself** — REST/HTTP today, pluggable transports next

The core broker speaks NGSI-LD; the plugins decide *where data lives*, *what extra
endpoints exist* and *how the broker talks to the world*.

- **Product version:** 0.3
- **Spec:** ETSI GS CIM 009 v1.9.1 (NGSI-LD) — fully implemented
- **Language / build:** C, CMake (wrapped by a convenience `makefile`)
- **License:** [Apache License 2.0](LICENSE) — Copyright 2026 Seamware

This project is part of [FIWARE](https://www.fiware.org/). For more information check
the FIWARE Catalogue entry for
[Core Context Management](https://github.com/FIWARE/catalogue/tree/master/core).

Questions, bugs and feature requests all belong in
[GitHub issues](https://github.com/SEAMWARE/coraine/issues) — that is where the
maintainers are. General FIWARE questions also reach people under the
[`fiware`](https://stackoverflow.com/questions/tagged/fiware) tag on Stack
Overflow.

> <sup>\*</sup> **On that 100%:** the conformance runs use a *corrected fork* of
> the ETSI test suite. The changes are test-side fixes — the suite has bugs of its
> own and parts of it simply don't run as published — never relaxations of what
> the broker must do. The fixes are filed upstream with ETSI.

---

## Table of contents

- [Footprint and speed](#footprint-and-speed) ← **start here**
- [Plugin architecture](#plugin-architecture)
- [Building](#building)
- [Running](#running)
- [API walkthrough](#api-walkthrough)
- [Documentation](#documentation)
- [Project layout](#project-layout)
- [Testing](#testing)
- [Quality assurance](#quality-assurance)
- [Training](#training)
- [Contributing](#contributing)
- [License](#license)

---

## Footprint and speed

coraine is a **1 MB broker**. Not a 1 MB container image with a runtime inside —
a 967 KiB stripped ELF binary that starts in 10 milliseconds and answers NGSI-LD
requests on the 11th.

### Size

Release build, stripped, x86-64:

| Artifact | Size |
|----------|-----:|
| `coraine` — the broker | **967 KiB** |
| `corDB.so` — in-memory DB | 39 KiB |
| `mongoc.so` — MongoDB DB | 116 KiB |
| `none.so` / `ramdb.so` — TRoE | 14 KiB each |
| `timescale.so` — TRoE | 67 KiB |
| `admin.so` — admin API | 23 KiB |

A complete, self-sufficient broker — binary + in-memory store + TRoE-off — is
**~1.0 MiB** on disk and needs no external service at all.

Those figures are the code coraine itself ships. The shared libraries it links —
`libmicrohttpd`, `libssl`/`libcrypto`, `libmosquitto` (MQTT notifications), ICU
(orderBy collation), GEOS (geo-queries, via the DB plugin) — are mapped by the
loader on top, and dominate the resident set: 14 MB RSS, of which only 2.6 MB is
private and dirty.

### Start-up

From `exec` to a listening socket, median of five, and the resident set right
after:

| Configuration | Ready in | RSS |
|---------------|---------:|----:|
| `--database corDB` | **10 ms** | 14 MB |
| `--database mongoc` (localhost Mongo) | **17 ms** | 21 MB |

That is fast enough that the broker is not something you keep warm — it is
something you start. Scale-to-zero, per-test instances, one broker per tenant on
a gateway: all of them stop being awkward at 10 ms and 14 MB.

### Throughput

32-core x86-64, `wrk -t8`, `GET /ngsi-ld/v1/entities?type=Vehicle&limit=20` over
100 preloaded five-attribute entities (~550 B each, ~11 KB per response), median
of three 5 s runs:

| DB plugin | c50 req/s | p99 | c200 req/s | p99 |
|-----------|----------:|----:|-----------:|----:|
| `mongoc` | 24 500 | 3.0 ms | 23 100 | 9.9 ms |
| `corDB` | 30 400 | 2.6 ms | 28 800 | 9.9 ms |

At 20 entities per response that is **~490 000 entities/s** against MongoDB and
~610 000 in memory. Single-entity retrieve (`GET /entities/{id}`, corDB, c50):
**181 000 req/s, p99 766 µs**.

> Numbers are from one machine and one shape of request — reproduce them on yours
> before quoting them. What travels is the shape: sub-millisecond work per
> request, and a broker that saturates cleanly. Quadrupling concurrency from 50
> to 200 costs 5% of throughput and multiplies p99 by 3.8 — which is what the
> extra queue alone accounts for. The additional clients wait; they do not make
> the broker slower at serving the ones already there.

### Compiling out what you don't need

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

---

## Plugin architecture

> **This is the heart of coraine.** The broker binary holds the NGSI-LD protocol
> logic, the REST layer, the JSON-LD engine and the subscription matcher — and
> **no storage code, no temporal code**. Those, plus any non-NGSI-LD admin/ops
> endpoints, are shared libraries loaded at startup. You pick them on the command
> line; you can write your own without touching the core.

| Category | Selected with | Active at a time | Bundled |
|----------|---------------|------------------|---------|
| **Current-state DB** | `--database` / `-db` | one | `mongoc` (default), `corDB` |
| **History DB (TRoE)** | `--troe` | one | `none` (default), `ramdb`, `timescale` |
| **API services** | `--apiPlugins` / `-api` | any number | `admin` |
| **Communication protocol** | — | REST/HTTP, built in | *planned* |

**Why that matters, beyond tidiness.** The broker never talks to a database. It talks
to a *driver interface* — `DbDriver.h` for current state, `TroeDriver.h` for history —
and those two headers are the entire contract, function by function, documented
semantics included. So bringing coraine to a store it has never seen is writing one
shared library against a documented header. It is not forking a broker, not patching
a query builder, and not learning NGSI-LD: the protocol, the JSON-LD engine and the
subscription matcher stay in the core, and the driver is only ever asked storage
questions.

Two properties make that practical rather than aspirational. A **NULL function
pointer means "unsupported"**, answered as `501 Not Implemented`, so a new backend can
ship the day it does entity CRUD and grow snapshots, registrations or context
persistence later — `corDB` legitimately ships without persistence on exactly that
basis. And the choice is made **at startup, not at build time**: the same binary runs
on MongoDB in production, in RAM for a test, and on your own store in the field,
because `--database` takes a path as readily as a name.

The full story — where plugins are resolved from, how the loader works, the driver
interfaces, plugin-contributed CLI args, and how to write your own — is in
[`doc/plugin-architecture.md`](doc/plugin-architecture.md).

---

## Building

coraine links a constellation of sibling repos (k-libs + Cor-Libs) plus several
system libraries. The repos must sit as **siblings** under one parent (default
`~/git`), because the build references `../<lib>/lib<lib>.a`.

### Fastest path — bootstrap script

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

### Dependency stack

- **k-libs** (gitlab.com/kzangeli): `kbase kalloc klog khash kjson kargs ktrace kprom`
- **Cor-Libs** (github.com/SEAMWARE): `corRest corNgsild corJsonld corPlugin`
- **umbrella / test runner**: `corLibs`, `corTest`

`make` auto-rebuilds `corRest`/`corNgsild`/`corJsonld` (the broker's `libs` target);
the k-libs and `corPlugin` must already be built (the umbrella or bootstrap handles
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

> Don't need Mongo? Build without it: `cmake -DCOR_FEATURE_MONGOC=OFF` and run with
> `--database corDB`. The mongo-c v2 driver is the most common build snag.

### Make targets

| Target | Effect |
|--------|--------|
| `make` / `make release` | Release build (`BUILD_RELEASE/`) |
| `make debug` | Debug build (`BUILD_DEBUG/`) |
| `make i` / `make di` | release/debug **+ install** |
| `make ci` / `make cdi` | clean + the above |
| `make install` | copy broker + plugins → `/usr/local/bin`, `/opt/seamware/plugins`, `/opt/seamware/etc` |
| `make clean` | remove build trees |
| `make test` | run the functional test suite (`corTest`) |
| `make coverage` | unit coverage report (`coverage/index.html`) |
| `make coverage-etsi` | full ETSI TP suite coverage (`coverage-etsi/index.html`) |

Install writes to `/opt/seamware/...` and `/usr/local/bin` — run with appropriate
permissions or pre-create the dirs.

Build-time feature selection lives in
[Compiling out what you don't need](#compiling-out-what-you-dont-need).

---

## Running

Default listen port is **1026**. Plugins default to `mongoc` (DB) + `none` (TRoE),
no API plugins.

```sh
# In-memory, pretty JSON, admin API on — zero external services:
coraine --database corDB --troe none --apiPlugins admin -pp 2

# Default (Mongo) on a custom port:
coraine --port 1027 --database mongoc

# Full plugin help (includes the selected plugins' own args):
coraine --apiPlugins admin --database mongoc --usage
```

Selected common options (`--usage` for the full list):

| Option | Default | Meaning |
|--------|---------|---------|
| `--port` / `-p` | 1026 | TCP listen port |
| `--database` / `-db` | `mongoc` | DB plugin (short name or path) |
| `--troe` / `-troe` | `none` | TRoE plugin (`none` disables history) |
| `--apiPlugins` / `-api` | — | comma-separated API plugins |
| `--foreground` / `-fg` | — | accepted, no effect — the broker always runs in the foreground |
| `--pretty-print` / `-pp` | 0 | JSON indent (0 = compact) |
| `--localOnly` / `-local` | off | disable distributed operations |
| `--defaultUserContext` / `-duc` | — | default `@context` URL |
| `--corsOrigin` | — | enable CORS (`__ALL` for any origin) |
| `--maxRequestSize` / `-mrs` | 2 | max body MiB (§ 6.3.2; 0 = no cap) |

---

## API walkthrough

coraine speaks NGSI-LD under `/ngsi-ld/v1`. Start a broker that needs nothing else,
create an entity, and read it back:

```sh
coraine --database corDB --troe none --apiPlugins admin -pp 2 &

curl -X POST http://localhost:1026/ngsi-ld/v1/entities \
  -H 'Content-Type: application/json' \
  -d '{ "id": "urn:ngsi-ld:Vehicle:A100", "type": "Vehicle",
        "brand": { "type": "Property", "value": "Mercedes" },
        "speed": { "type": "Property", "value": 80 } }'

curl 'http://localhost:1026/ngsi-ld/v1/entities?type=Vehicle&q=speed>50'
```

Queries, the three representations (`normalized`, `concise`, `keyValues`), updates,
subscriptions and notifications are walked through in
[`doc/api-walkthrough.md`](doc/api-walkthrough.md).

---

## Documentation

| Document | What it covers |
|----------|----------------|
| [Installation & Administration](doc/installation.md) | dependencies, build, install, every option, the admin API, tenants |
| [API walkthrough](doc/api-walkthrough.md) | the API by example, from create to subscribe |
| [Plugin architecture](doc/plugin-architecture.md) | the plugin categories, the loader, the driver interfaces, writing your own |
| [Test coverage](doc/coverage.md) | what the suite covers, per DB, and what is left |
| [Functest coverage of the spec](doc/spec-coverage-gaps.md) | every spec statement, and whether a test asserts it |
| [Roadmap](doc/roadmap.md) | where coraine is going |

The full API is the specification itself: **ETSI GS CIM 009 / TS 104 175**, which
coraine implements in full. Every command-line option is listed by
`coraine --usage`, including the options contributed by the plugins you selected.

---

## Project layout

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

---

## Testing

Functional tests run through `corTest` (installed by the `corLibs` umbrella into
`~/git/corLibs/bin/corTest`):

```sh
make test                    # whole suite (mongoc; use corTest -db corDB for in-memory)
```

Tests live under `test/funcTests/`.

### Coverage

```sh
make coverage                # corDB   → coverage-corDB/index.html
make coverage DB=mongoc      # mongoc  → coverage-mongoc/index.html
make coverage-etsi           # ETSI TP suite → coverage-etsi/index.html
```

Measured **2026-08-20** on `e8e6bb6`:

| Run | Tests | Lines | Functions | Branches |
|-----|-------|-------|-----------|----------|
| `DB=mongoc` | 613 / 613 pass | 80.7% | 93.7% | **60.8%** |
| `DB=corDB` | 563 / 563 pass | 74.4% | 85.7% | **55.4%** |

Branch coverage is the honest number of the three, and the one to move. Of the
uncovered lines, roughly **one in six** is a failure path no test can reach without
fault injection — a database that fails on demand, a socket that dies mid-write —
and the rest is reachable code nobody has written a test for yet, most of it
distributed-operation forwarding and batch partial-success assembly.
[`doc/coverage.md`](doc/coverage.md) has the full breakdown, the method behind it,
and why the two runs are separate measurements rather than two views of one.

The ETSI target instruments the broker **and** the NGSI-LD libs (whole-archived,
so they flush through the broker's gcov runtime) **and** the mongoc/timescale
plugin `.so`s.

---

## Quality assurance

- **Conformance:** 100% of the official ETSI NGSI-LD conformance test suite
  (see the note at the top of this file).
- **Functional tests:** 613 tests against MongoDB, 563 against the in-memory store,
  run through `corTest`. A change in behaviour is not finished until a test pins it.
- **Coverage:** measured per DB and published in [`doc/coverage.md`](doc/coverage.md),
  along with an estimate of how much of what is left can only be reached by making
  the environment fail.
- **Memory safety:** the harness can run the whole suite with the broker under
  valgrind (`corTest -vt`), failing on definite or indirect leaks and on valgrind
  errors.

The FIWARE GE ratings badges (documentation completeness, responsiveness, FIWARE
testing) are published from the Catalogue once the entry exists; they will be added
here at that point.

---

## Training

| [Documentation](https://coraine.readthedocs.io/) | [FIWARE Academy](https://fiware-academy.readthedocs.io/) | [NGSI-LD Tutorials](https://ngsi-ld-tutorials.readthedocs.io/) | [FIWARE Catalogue](https://www.fiware.org/developers/catalogue/) |
| --- | --- | --- | --- |

The NGSI-LD tutorials apply to coraine unchanged — it implements the same API. The
[step-by-step guide](doc/api-walkthrough.md) in this repository is the shortest path
from a running broker to a working subscription.

---

## Contributing

Contributions are welcome. [`CONTRIBUTING.md`](CONTRIBUTING.md) describes the terms —
including the Individual Contributor License Agreement that every pull request must
carry — how to build and test, and what a good bug report contains. Participation is
governed by the [Code of Conduct](CODE_OF_CONDUCT.md).

The backlog is [`ToDo.md`](ToDo.md): what is not built yet, and what is deferred by
design.

---

## License

coraine is licensed under the [Apache License 2.0](LICENSE) — Copyright 2026 Seamware.
Every source file carries an `SPDX-License-Identifier`. The people and projects it is
built on are named in [CREDITS.md](CREDITS.md).
