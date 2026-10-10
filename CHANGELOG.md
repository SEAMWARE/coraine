# Changelog

Notable changes per release. The entry for a version is what its
[GitHub release](https://github.com/SEAMWARE/coraine/releases) page shows, so it is
written for someone deciding whether to upgrade — not a copy of the git log.

Versions follow [semantic versioning](https://semver.org/). Below 1.0.0 a minor
bump may still change behaviour; the entry says so where it does.

---

## 0.5.0 — 2026-10-10

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
- **Paging that holds while the data changes** — a local query pages by position: its
  `next`/`prev` links carry the query and the place of the page's last/first entity
  (`pageAfter` / `pageBefore`), so deleted entities or entities that stop matching never make
  a page skip or repeat one, and a deep page costs what the first does. A **distributed**
  query freezes its set in an EntityMap the broker creates itself (TS 104-175 § 9.6) and
  fetches each page with one request per Context Source. `entityMap=true` freezes a local set
  on request. The map is named in the `NGSILD-EntityMap` header, which a client may also send
  instead of `?entityMap=<id>`.

### Faster

- Requests that wait run as **coroutines** of the event loops, not on worker threads.
- **Subscriptions at scale** — a write is matched only against the subscriptions that can
  match it: +32 % with 1 000 subscriptions.
- **mongoc: a query by type** reads only the page it returns (an index on
  `{type, createdAt, _id}`): ×19.7 for a type that is 1 % of the store.
- An NGSI-LD-aware JSON parser: the core terms are an enum, not strings.
- **Deep pages**: a page at depth 1 000 by position instead of offset - ×3.3 on mongoc
  (3 485 → 11 301 req/s), ×65 on corDB (243 → 15 958). A page served from an EntityMap is one
  store call (one query on mongoc instead of one per entity).

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
- **Service Execution**: a service name is unique within an entity - a registration that would
  give an entity the same name twice is refused (409), and an invocation by a name two older
  registrations share answers 409 instead of picking one. `scopeQ` selects by the entity's scope
  on registrations and grouped executions. Grouped and combined executions and Combined Service
  Templates refuse a member they do not know (400) instead of ignoring it.
- **Paged queries**: a local query's `next`/`prev` links carry `pageAfter` / `pageBefore`
  instead of `offset` (a client's own `offset` works as before). A distributed query with more
  than one page is served from an automatic EntityMap: its links carry `entityMap=<id>` with
  the original query, an entity that stops matching is left out of its page, a request on a map
  with a different query is refused (400), an expired map is recreated from the request; an
  automatic map answers 200 (201 only when the client asked for one).
  `--autoEntityMaps none|distributed|all` (default `distributed`) and `--entityMapMemory`
  (default 64 MiB) bound them.
- **Downgrading to 0.4.x is not supported once 0.5.0 has written to a mongoc database.** 0.5.0
  stores the system timestamps and attribute types once per entity; 0.4.x reads attributes
  0.5.0 has written back without `type` and `createdAt`. 0.4.x cannot detect this - do not run it
  on data 0.5.0 has written.
- **A store records its storage format**, and a broker refuses to start on a store written in a newer
  format than it knows: mongoc in each database (`metadata`, `{_id: "storageFormat", version}`),
  corDB in `<--dbDir>/_storageFormat`, timescale in its schema version. A database without one (0.4.x)
  is read as it is and given 0.5.0's (`doc/installation.md`, "Storage format").
- **A DB or TRoE plugin and the broker check each other's interface stamp** at load - a hash of the
  headers they share structs through - and refuse to start on a mismatch, naming both stamps. A
  broker and plugins built from different sources no longer load and corrupt memory
  (`doc/plugin-architecture.md`, "The DB plugin interface stamp").
- **The built-in HTTP server no longer shares its port** (`SO_REUSEPORT` is off): a second broker on
  the same port fails with "Address already in use" instead of taking part of the connections.
- **`make install`** replaces each installed file (a new file renamed over the old) instead of
  writing into it: a broker running from the installed plugins keeps running.
- **`coraine-build --list-features --json`**, and one `coraine-build: phase <name>` line per phase.
- **WebSocket works like HTTP**: a connection has no tenant nor any other NGSI-LD header - every
  message carries its own in `metadata`, and a message without `NGSILD-Tenant` is for the default
  tenant. An upgrade request carrying `NGSILD-Tenant`, `Link` or `NGSILD-Snapshot` is refused (400).
- **A request in flight when the broker stops** (built-in HTTP server) is answered as failed and
  freed; a notification in flight then is counted as failed.
- **Discovery** (`/attributes`, `/attributes/{attr}`, `/types`, `/types/{type}`): a core attribute's `id`
  is its full IRI (`https://uri.etsi.org/ngsi-ld/location`, `.../observationSpace`, `.../operationSpace`,
  ...), as TS 104 175 § 5.2.6.10.1 requires; `attributeName` stays compacted.
, default `/opt/seamware/etc`): where the broker
  looks for `bridges.json` and `contextSourceExtras.json` when no file is named.

### Fixed (a selection)

- A grouped Service Execution ignored `scopeQ` and ran the service on entities outside the
  scope.
- A GeoProperty with an altitude was refused (400) by corDB built against GEOS < 3.13
  (Ubuntu 24.04); the altitude is kept.
- A request arriving in the first milliseconds after start ran outside the worker pool.
- `make release` linked debug foundation libraries; every release build now checks it
  carries no debug code.
- A `>` in a query (`q=speed>20`) broke the pagination `Link` header; link values are now
  percent-encoded.
- `?entityMap=true` copied itself into the `next` link, creating a new map on every page.
- A registration with 14 or more `contextSourceInfo` entries crashed the broker on the first
  forwarded request (a request with more than 15 headers freed memory it did not own).
- cor:// client connections were not released when a thread ended (a buffer and an open socket
  per thread).
- Periodic and re-dispatched notifications sent a `Link` header from a buffer that had gone out of
  scope.
- Allocations were not 8-byte aligned (undefined behaviour; a fault risk for atomic accesses on
  arm64).
- The built-in HTTP server leaked each thread's pool of finished coroutines when the thread ended.
- A bridge's sample text was parsed in place: the plugin's buffer came back truncated, and a string
  literal crashed the broker.
- Without `--bridgeConfig`, a bridge plugin was not given the default configuration file the
  broker itself read (MQTT: "no server").
- The MQTT and Modbus bridges read at most 64 KiB of their configuration and ignored the rest.
- A forwarded request's response (body, status text, headers) pointed into its connection's buffer,
  which could be reused by another request or freed before it was read - distributed operations,
  forwarding, Service Execution executors, snapshots and the @context download.
- A forwarded request whose path and query were longer than about 450 characters failed (502).
- An HTTPS request through the multi client could wait for its full timeout with its answer already
  read (data left inside OpenSSL).
- Distributed discovery counted an attribute twice when a source answered its full IRI.
- An environment-variable prefix ending in `_` gave option variables a double underscore.
- A deleted Snapshot's store was never freed (corDB), nor its geo index cache (mongoc); deleting a
  Snapshot while corDB was writing its own snapshot of the store could read freed memory.
- Coroutines and connections still waiting when the built-in server stopped were never freed.
- The image's health check asked port 1026 whatever the broker was started with: a container broker
  on another `--port` was marked unhealthy. It now asks the broker's own port (`/live` on the health
  port when there is one).

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
