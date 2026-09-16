# coraine

[![FIWARE Core Context Management](https://fiware.github.io/catalogue/badges/chapters/core.svg)](https://www.fiware.org/developers/catalogue/)
[![License badge](https://img.shields.io/github/license/SEAMWARE/coraine.svg)](https://opensource.org/licenses/Apache-2.0)
[![Container badge](https://img.shields.io/badge/quay.io-seamware%2Fcoraine-grey?logo=red%20hat&labelColor=EE0000)](https://quay.io/repository/seamware/coraine)
[![Support badge](https://img.shields.io/badge/support-github%20issues-orange.svg?logo=github)](https://github.com/SEAMWARE/coraine/issues)
[![NGSI-LD badge](https://img.shields.io/badge/NGSI-LD-red.svg)](https://www.etsi.org/technical-groups/data/)
<br/>
[![Release badge](https://img.shields.io/github/v/release/SEAMWARE/coraine?label=release)](https://github.com/SEAMWARE/coraine/releases)
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
- **outbound transports** — REST/HTTP always, other protocols next (the *bridge* seam)

The core broker speaks NGSI-LD; the plugins decide *where data lives*, *what extra
endpoints exist* and *how the broker talks to the world*.

- **Product version:** 0.4 — see the [release notes](https://github.com/SEAMWARE/coraine/releases)
  and the [changelog](CHANGELOG.md)
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

- [Quick start](#quick-start) ← **start here**
- [Footprint and speed](#footprint-and-speed)
- [Plugin architecture](#plugin-architecture)
- [Running](#running)
- [API walkthrough](#api-walkthrough)
- [Documentation](#documentation)
- [Quality assurance](#quality-assurance)
- [Training](#training)
- [Contributing](#contributing)
- [License](#license)

---

## Quick start

A published image, an in-memory store, no external services. One command, and the
broker answers NGSI-LD on port 1026:

```sh
docker run --rm -p 1026:1026 \
    quay.io/seamware/coraine:0.4.0 --database corDB
```

Then, from another terminal — create an entity and read it back:

```sh
curl -X POST localhost:1026/ngsi-ld/v1/entities \
     -H 'Content-Type: application/json' \
     -d '{
           "id":   "urn:ngsi-ld:Sensor:1",
           "type": "Sensor",
           "temperature": { "type": "Property", "value": 21.5 }
         }'
# 201 Created

curl localhost:1026/ngsi-ld/v1/entities/urn:ngsi-ld:Sensor:1
# {"id":"urn:ngsi-ld:Sensor:1","type":"Sensor",
#  "temperature":{"type":"Property","value":21.5}}
```

That is the whole broker — `corDB` keeps entities in RAM, so nothing else has to
be installed or configured. Swap in `--database mongoc --dbHost <host>` when the
data should outlive the process; see [Running](#running).

Building it yourself instead — the dependency stack, the system packages, the make
targets — is [Building from source](doc/building.md).

**Images are at [quay.io/seamware/coraine](https://quay.io/repository/seamware/coraine)**,
tagged `<version>-<date>-<commit>` — one immutable tag per merge to `main`, never
expiring. There is deliberately **no `latest`**: a tag that moves under a running
deployment is a version nobody can name afterwards. Pick the newest from the tag
list, or pin the one you tested.

---

## Footprint and speed

A broker is not an executable. It is everything that has to be on the machine
before it can answer: the binary, the plugins it loads, the libraries that were
not there before it arrived, and any other server it needs running. That is what
is counted here. A small `main` on top of large libraries is not a small broker,
and quoting the `main` would be the wrong number.

Four builds, the two axes that change what a coraine process is made of — the
HTTP server (`corHttp`, built in, or the external libmicrohttpd) and the
current-state DB (`corDB`, entities in this process's RAM, or `mongoc`, entities
in a MongoDB server). **Every rate is per core**: the broker is pinned to one
physical core and the load generator kept off it.

| Build | Disk added | RAM idle | RAM · 100 k entities | Start-up | req/s per core | entities/s per core |
|-------|-----------:|---------:|---------------------:|---------:|---------------:|--------------------:|
| `corHttp` + `corDB` | **4.3 MiB** | 17 MiB | 354 MiB | 12 ms | 5 901 | 118 020 |
| `corHttp` + `mongoc` | 11.1 MiB | 24 MiB | 45 MiB *+ mongod* | 31 ms | 4 634 | 92 680 |
| libmicrohttpd + `corDB` | 11.6 MiB | **13 MiB** | 358 MiB | **9 ms** | **6 588** | **131 760** |
| libmicrohttpd + `mongoc` | 18.4 MiB | 20 MiB | 68 MiB *+ mongod* | 26 ms | 4 904 | 98 080 |

<sub>AMD Ryzen 9 8940HX laptop, 16 physical cores, Ubuntu 26.04, release build,
`COR_FEATURE_ICU_COLLATION=OFF`. `GET /entities?type=Vehicle&limit=20`, 20
entities per response. **Disk added** counts only what a bare `ubuntu:26.04`
does not already carry. `mongod` adds 1.02–1.15 GiB resident and cores of its
own; `corDB` adds nothing.</sub>

Five things worth taking from that table:

- **A complete NGSI-LD broker, in-memory store included, is 4.3 MiB of files a
  machine did not already have — and three libraries, two of which are GEOS.**
  1.00 MiB of it is coraine, and the cor and k libraries are whole-archived into
  that binary, so it is not a `main` calling out to something else: `corNgsild`,
  `corRest`, `corJsonld`, `kjson`, `kalloc` and the rest are *in* the megabyte.
- **`corDB` + `ramdb` needs no other service at all** — and temporal history in
  the same process is **free**: 40 257 req/s against 40 073 with history off,
  123 307 PATCH/s against 125 187. History in PostgreSQL costs `corDB` 91% of
  its PATCH rate instead.
- **The page size is the claim.** 6 588 requests/s per core at `limit=20` is
  **131 760 entities/s per core**; at `limit=1` it is 37 743 of each; at
  `limit=100` it is **158 300 entities/s**. A requests/s figure without the
  response size beside it means nothing.
- **Batching is worth three to four times per entity.** A `PATCH` one at a time
  is 42 234 entities/s per core; twenty per request is 131 260. Creates:
  31 362 against 113 640.
- **The database is the bill.** One broker core doing batch updates through
  MongoDB needs **four mongod cores behind it** before MongoDB stops being the
  limit — five cores of machine to do what `corDB` does on one. On eight cores
  shared by everything a configuration needs, `corDB` serves 40 073 req/s and
  `mongoc` 23 185.

📊 **[Performance and footprint](doc/performance.md)** has the rest: what is
inside the megabyte, the per-client latency curve, what MongoDB costs in cores
and RAM, why ICU is off, an open question about `--connectionPoolSize`, and how
every number was measured.

### Compiling out what you don't need

In principle the broker shrinks to exactly the NGSI-LD you deploy: no subscription
engine on a read-only edge node, no registrations, no `datasetId`, no geo, no
tenants, no Mongo. The switches are `COR_FEATURE_*` at build time, which means
**building it yourself** — a published image is compiled with everything on.

⚠ Be warned before planning around it: **the flags are declared, the work behind
them is partly done.** `-DCOR_FEATURE_MONGOC=OFF` and
`-DCOR_FEATURE_ICU_COLLATION=OFF` (the reference build above) genuinely work, and
the subscription and registration engines now compile out; several of the
remaining flags are still declarations with no `#ifdef` behind them, and turning
one of those off changes nothing or fails the link.
[Building from source](doc/building.md#compiling-out-what-you-dont-need) has the
full list and the honest state of each.

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
| **Bridge** (outbound transport) | endpoint scheme | per scheme | HTTP/HTTPS built in; others *planned* |

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

### API specification

The API is not ours to define. coraine implements **ETSI GS CIM 009 v1.9.1**, and
the normative definition is the ETSI deliverable itself — restating it here would
only create a second copy to keep in sync, and the one that drifted would be ours.

The machine-readable form is published by ETSI ISG CIM, **and it currently lags
the specification**, which is worth knowing before generating a client from it:

- **OpenAPI 3.0.3, bundled and self-contained** —
  [`full_api.json`](https://forge.etsi.org/rep/cim/NGSI-LD/-/raw/master/spec/updated/generated/full_api.json)
  on ETSI Forge, publicly readable with no account. It declares its version as
  `latest` but was last updated in **April 2022**, so it describes roughly
  v1.7.1–1.8.1 rather than the v1.9.1 implemented here. An ETSI Specialist Task
  Force is producing a current one.
- **The same definition split by resource** —
  [`spec/updated`](https://forge.etsi.org/rep/cim/NGSI-LD/-/tree/master/spec/updated),
  with `ngsi-ld-spec-open-api.json` as the root document.
- **The companion deliverable** — ETSI GS CIM 047, *"Context Information
  Management (CIM); OpenAPI Specification for NGSI-LD API"*.

So for anything added since 1.8.1 — and that includes much of what this broker
implements — the specification document is the authority, not the OpenAPI file.

Where coraine deliberately differs from the specification, or where the
specification is ambiguous and we had to choose, it is written down rather than
left to be discovered: see [`doc/spec-coverage-gaps.md`](doc/spec-coverage-gaps.md).

---

## Documentation

**Rendered and searchable at [coraine.readthedocs.io](https://coraine.readthedocs.io)**,
rebuilt from `main` on every merge. The same pages live under `doc/` in this
repository, which is where to read them offline or alongside a checkout:

| Document | What it covers |
|----------|----------------|
| [Installation & Administration](doc/installation.md) | dependencies, build, install, every option, the admin API, tenants |
| [Performance and footprint](doc/performance.md) | what it costs on disk and in RAM, per-core throughput, and how each number was measured |
| [API walkthrough](doc/api-walkthrough.md) | the API by example, from create to subscribe |
| [Plugin architecture](doc/plugin-architecture.md) | the plugin categories, the loader, the driver interfaces, writing your own |
| [Building from source](doc/building.md) | the source layout, the dependency stack, system packages, make targets, compiling features out |
| [Testing](doc/testing.md) | running the suite, and measuring coverage |
| [Speaking to devices directly](doc/device-protocols.md) | reaching devices without an IoT Agent tier, and what that needs |
| [FIWARE IoT Agents](doc/iot-agents.md) | what they do, how they integrate, and where the boundary sits |
| [Test coverage](doc/coverage.md) | what the suite covers, per DB, and what is left |
| [Functest coverage of the spec](doc/spec-coverage-gaps.md) | every spec statement, and whether a test asserts it |
| [Roadmap](doc/roadmap.md) | where coraine is going |

The full API is the specification itself: **ETSI GS CIM 009 / TS 104 175**, which
coraine implements in full. Every command-line option is listed by
`coraine --usage`, including the options contributed by the plugins you selected.

---

## Quality assurance

- **Conformance:** 100% of the official ETSI NGSI-LD conformance test suite
  (see the note at the top of this file).
- **Functional tests:** 634 tests against MongoDB, 584 against the in-memory store,
  run through `corTest`. A change in behaviour is not finished until a test pins it.
- **Coverage:** measured per DB, run as described in [`doc/testing.md`](doc/testing.md)
  and published in [`doc/coverage.md`](doc/coverage.md),
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

C style for the whole stack is one document:
[`STYLE_GUIDE.md`](https://github.com/SEAMWARE/corLibs/blob/main/STYLE_GUIDE.md) in
the `corLibs` umbrella.

The backlog is [`ToDo.md`](ToDo.md): what is not built yet, and what is deferred by
design.

---

## License

coraine is licensed under the [Apache License 2.0](LICENSE) — Copyright 2026 Seamware.
Every source file carries an `SPDX-License-Identifier`. The people and projects it is
built on are named in [CREDITS.md](CREDITS.md).
