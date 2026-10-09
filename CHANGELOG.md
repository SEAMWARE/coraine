# Changelog

Notable changes per release. The entry for a version is what its
[GitHub release](https://github.com/SEAMWARE/coraine/releases) page shows, so it is
written for someone deciding whether to upgrade — not a copy of the git log.

Versions follow [semantic versioning](https://semver.org/). Below 1.0.0 a minor
bump may still change behaviour; the entry says so where it does.

---

## 0.5.0 — 2026-10-09

Six weeks of work since 0.4.0: a broker that keeps its data on disk with no database
server, temporal history in the same process, its own binary protocol between brokers,
bridges to DDS, MQTT and Modbus, Service Execution - and everything it takes to run it
in production: arm64, Kubernetes, a migration path from Orion-LD and Debian packages.

### New

- **corDB on disk** — `--dbDir`: an append log synced every 100 ms and snapshots; the
  store survives a restart, still with no database server. A clean stop (SIGTERM) loses
  nothing.
- **Temporal history in corDB** — `--troe corDB`: the whole temporal API on history corDB
  keeps itself, on disk with the store - no PostgreSQL.
- **`cor://`** — a binary protocol for broker-to-broker traffic: forwarding without a JSON
  parse, many requests in flight on one connection.
- **Bridges and Channels** — DDS (topics both ways, services, actions; ROS 2), MQTT (device
  topics as Channels, notifications to `mqtt://`) and Modbus TCP (registers as Channels).
  coraine's own mechanism, not (yet) a standard.
- **Service Execution** — "do this" in the API: Service Descriptions, executions,
  synchronous and asynchronous (GR CIM-055).
- **Snapshots** — § 5.16 complete, writes and subscriptions on a Snapshot, on both stores.
- **The WebSocket transport** — subscriptions and notifications over a WebSocket.
- **Array reduction of entity data** — one-element arrays reduced at the input, as the
  @context says (`@set` / `@list`).
- **linux/arm64** — every image for amd64 and arm64 under one tag (arm64 without the DDS
  bridge); the whole functional suite runs on ARM every night.
- **Migrating from Orion-LD** — `coraine-import`, a program of its own: entities,
  subscriptions, registrations and the temporal history, with their ids and system
  timestamps (`doc/migration.md`).
- **Debian packages** — `apt install coraine` (and `coraine-dev`: the exact source and
  `coraine-build`) for Ubuntu 26.04, 24.04 and Debian 13, amd64 and arm64, from
  https://seamware.github.io/apt.
- **Kubernetes** — `--healthPort`: `/live`, `/ready` and a report on a port of its own,
  answered outside the HTTP queue; a memory budget that refuses work instead of being
  OOM-killed; a byte budget per query response (`doc/kubernetes.md`).
- **`make tune`** — a broker built for your workload: the profile trained on it, the
  allocator and the lock policy measured on it.
- **EntityMaps the broker creates itself** — a query whose answer is more than one page
  freezes its set of matching entities and serves every page from it (TS 104-175 § 9.6):
  consistent pages while the data changes, and correct paging of distributed queries. The
  map is named in the `NGSILD-EntityMap` header, which a client may also send instead of
  `?entityMap=<id>`.

### Faster

- Requests that wait run as **coroutines** of the event loops, not on worker threads.
- **Subscriptions at scale** — a write is matched only against the subscriptions that can
  match it: +32 % with 1 000 subscriptions.
- **mongoc: a query by type** reads only the page it returns (an index on
  `{type, createdAt, _id}`): ×19.7 for a type that is 1 % of the store.
- An NGSI-LD-aware JSON parser: the core terms are an enum, not strings.

### Changed behaviour

- **`--maxResponseSize`** (new): a query stops fetching at a byte budget - entity, temporal
  and distributed queries alike: a page shorter than `limit` with a correct `next` link, a
  temporal history cut as the instance cap cuts it (TS 104-176 § 6.4.7.3 pagination), a
  Context Source's answer read only up to the budget, or 403 TooManyResults where a partial
  answer is impossible. By default 1/16 of the memory budget; **no budget outside a container with a
  memory limit**, so nothing changes there.
- **The memory budget** counts anonymous memory (RssAnon + RssShmem), not the whole resident
  set: a persistent corDB is no longer refused for the size of its memory-mapped log. New
  metric `ngsild_memory_used_bytes`.
- **`--high-availability mongo`** refuses a temporal store inside the process
  (`--troe ramDB`): each instance would hold only part of the history.
- **The Docker `HEALTHCHECK`** asks `/version` instead of `/ngsi-ld/v1/types`.
- **Temporal representation:** the entity's `modifiedAt` (timescale) includes its
  attributes' updates.
- **Subscription / registration PATCH** follows TS 104-175 § 8.4.2: a first-level member
  is replaced whole.
- **Delete Entity** with an inclusive registration: a source answering 404 is not an error
  (204).
- **mongoc** drops its old `type_1` index at startup (replaced by `{type, createdAt, _id}`).
- **Paged queries**: a `GET /entities` with more matches than `limit` (and no `orderBy`) is
  served from an automatic EntityMap. Its links carry `entityMap=<id>` with the original
  query; an entity that stops matching is left out of its page (a page can be shorter than
  `limit`); a request on a map with a different query is refused (400); an expired map is
  recreated from the request. An automatic map answers 200 (201 only when the client asked
  for one). `--entityMapMemory` (default 64 MiB; 0 = no automatic maps) bounds them.

### Fixed (a selection)

- A GeoProperty with an altitude was refused (400) by corDB built against GEOS < 3.13
  (Ubuntu 24.04); the altitude is kept.
- A request arriving in the first milliseconds after start ran outside the worker pool.
- `make release` linked debug foundation libraries; every release build now checks it
  carries no debug code.
- A `>` in a query (`q=speed>20`) broke the pagination `Link` header; link values are now
  percent-encoded.
- `?entityMap=true` copied itself into the `next` link, creating a new map on every page.

## 0.4.0 — 2026-08-28

The first tagged release. Everything below already existed and had never carried a
version anyone could ask for by name — that is what this release is for, and why
the entry describes a surface rather than a diff.

### The broker

- **NGSI-LD, in full.** ETSI GS CIM 009 v1.9.1 — entities, subscriptions,
  registrations, batch operations, the temporal API, distributed operations across
  context sources, and JSON-LD `@context` handling.
- **Notifications over HTTP, HTTPS, MQTT and MQTTS.** MQTT QoS and protocol
  version are selectable per subscription through `endpoint.notifierInfo`
  (§ 7.2), and the MQTT broker may require TLS, a username and a password.
- **Written in C**, built with CMake. One process, no runtime, no JVM.

### Plugins, loaded at startup as shared libraries

- **Current state** — `mongoc` (MongoDB) and `corDB` (in-memory).
- **Temporal history** — `timescale` (TimescaleDB), `ramdb` (in-memory) and `none`.
- **API surfaces** — `admin`, for the endpoints that are not NGSI-LD: ops and
  administration, and **56 Prometheus metrics** on `/metrics` and
  `/admin/metrics` covering entity, batch, subscription, registration, context
  source and distributed-operation activity.

Which ones a build contains is a compile-time choice, and which ones it loads is a
run-time one.

Where coraine goes from here is on the
[roadmap](https://coraine.readthedocs.io/en/latest/roadmap/).
