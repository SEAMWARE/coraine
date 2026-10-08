# Ideas

Every idea for coraine in one place - what is planned and what is only possible. None of it is a
commitment; [the roadmap](roadmap.md) says which of these are next. The day-to-day backlog is
[the second half of it](roadmap.md#the-backlog-in-detail).

Each idea: one line, then what there is to say about it, and where more lives.

## Storage

### corDB persistence - cheaper writes

corDB persists with `--dbDir` (a log and snapshots - [corDB's design](https://github.com/SEAMWARE/corDB/blob/main/doc/persistence.md));
what it costs is in [Performance](performance.md), "corDB on disk": nothing on reads, 6-24 % on
writes that change the store, more while the store grows fast. Two ways to bring it down: a PATCH or
merge that logs only the attributes it touched (`ATTRS_PUT`, already in the record format) instead
of the whole entity, and a cheaper encode of a record (the per-request cost is the encode).

### corDB history: retention and a selector

`--troe corDB` keeps the temporal history in the corDB store itself - captured where corDB writes,
on disk with the store, the whole temporal API on it, no second plugin and no PostgreSQL. In-process
history costs nothing measured on eight shared cores; the same history in PostgreSQL costs corDB 91 % of
its write rate. Two things it does not have yet: **retention** - history only grows; kept for a time or
a size, whole old chunks dropped - and a **selector**: what history records, configurable and
subscription-shaped (entities, attributes, include or exclude), where today every write to every
attribute is recorded. Not built either, and not needed for those two: the persistence log as the
history itself (history is segments of its own today).

### The Entity as it was at time T

A current-state Entity at an instant in the past - not the temporal representation (every value with
its timestamps), the Entity as a retrieve would have answered at T. Discussed at ETSI, not in the
specification yet; implemented when it is. With corDB's log it is nearly free by system time (the
newest snapshot at or before T plus the log up to T); by `observedAt` it needs the temporal index per
attribute - and a late sample can change the answer later. [The roadmap, § 15](roadmap.md#15-persistence-one-log-and-history-as-a-retention-policy-on-it).

### corsh - the corDB shell

A maintenance tool (a general broker shell is for later), part of corDB's own repository once corDB
leaves this one: `stats`, `backup` (consistent while the broker runs - a snapshot,
then hard links), `compact`, `purge` by type or `q`, `drop` a tenant, `index add/drop/list`, and on a
stopped broker's files `verify`, `dump`, `export`/`import`. Live data goes through the broker (cor://
to the `admin` plugin); files only when no broker runs. Design: [corDB's design](https://github.com/SEAMWARE/corDB/blob/main/doc/persistence.md) § 10.

### corDB indexes

corDB indexes the entity id (a hash) and keeps entities in creation order - the default query order,
as with `mongoc`. A query by type, a `q` and a geo-query walk the tenant. A **type** index in creation
order (with persistence), **attribute-value** indexes for `q`, an **R-tree** for `georel` - each a
declaration kept with the tenant's data, rebuilt at load, measured before it stays.

### corDB standalone - and as haaux

corDB as a process of its own, beside the brokers, for high availability: already connected to every
broker of the group, it is what haaux needs (below). One codebase, built as the in-broker plugin (the
default) or the server, with a remote-corDB plugin as the broker's client of it; backups of current
state and history. (corDB already is a repository of its own, SEAMWARE/corDB.)

### A smaller CorNode

A prefix layout for the tree node: smaller nodes, less corDB memory, fewer cache misses walking a
tree. To be measured with `perf stat` cache-misses on the GET scenarios.

## Protocols and transports

### cor:// - what the format does not do yet

Packed numeric arrays and timestamps as integers (`cor-protocol-details.md` § 4.9, § 4.10); the wire
size of broker-to-broker traffic, with its expanded names, measured; Unix-domain sockets for brokers
on one host.

### Transports as plugins

HTTP and cor:// as protocol plugins, loaded like the database, TRoE and bridge plugins - and with that
an NGSIv2 API plugin, and NGSI-LD itself perhaps a plugin. No hurry.

### Coroutines - open questions

The cap (1024 per loop) and the stack size (256 KiB) as options; libmicrohttpd's own
`MHD_suspend/resume_connection` for coroutines, or the built-in server as the default; a yielding
mongoc stream (`mongoc_client_set_stream_initiator`) so `mongoc` requests can be coroutines too.
[Coroutines](coroutines.md) § 6.

## Bridges and devices

### More bridges

Kafka, WebSockets, OPC UA on the Bridge/Channel seam that carries DDS, MQTT and Modbus. The protocol
names the endpoint and the endpoint decides the transport; HTTP stays inline rather than a plugin,
because it is also the NGSI-LD API.

- **Kafka** - one standard binding across the brokers that support it.
- **WebSockets** - notifications to consumers that cannot be HTTP servers: behind NAT, a firewall, a
  browser.
- **OPC UA** - variables as attributes, monitored items as subscriptions, methods as Service
  Execution.

### Bridges and Channels over the API

Today they come from the configuration file and are read over `GET`; create, update and delete over
the API, persisted - the design is in `bridge-channels-details.md` (not published while the concept
is in front of ETSI).

### Aligning Bridges and Channels with ETSI

Expected in 2027, on whatever TC DATA standardises after the Athens face-to-face of October 2026.
Adaptation work rather than new capability: the objects, the endpoint-scheme convention and the codec
seam are likely to move, the transports carried over them are not. coraine's Bridge and Channel are
not standard NGSI-LD.

### Service Execution

Actuation as a first-class citizen of the API, beyond the suggested workflows of TS 104 175 Annex G.
DDS services and actions reach the broker today through Channels and a provisional convention (a
write to an attribute, goals as its instances); the broker still needs a way to say "do this" that is
not a write to an attribute.

### The IoT Agents as cor-agent plugins

Parity with the FIWARE IoT Agents - UltraLight, JSON, LWM2M, LoRaWAN, Sigfox, OPC UA, ISOXML - as
plugins on the bridge contract instead of separate processes. The same source compiles to a reduced
**cor-agent** configuration, so agent-then-broker becomes a deployment choice: a small edge build
beside a central broker, or one binary doing both (FIWARE@Home on a Raspberry Pi). More of them as
deployments ask, not to complete a matrix. [Speaking to devices directly](device-protocols.md),
[FIWARE IoT Agents](iot-agents.md).

### A smaller DDS stack for the DDS bridge

The DDS bridge runs on a full DDS stack, the largest part of a coraine installation (about five times
the broker) - so the arm64 image and the Debian packages ship without the bridge, and a device agent on
a small board could not carry it. What the bridge needs is a subset: discovery (SPDP/SEDP), reliable
and best-effort readers and writers, fragmentation and CDR serialisation - no content filters, no DDS
Security. Two ways, the cheaper first: a DDS implementation written in C, built behind the same plugin;
or that subset of the wire protocol (RTPS) on its own. Interoperability with DDS and ROS 2 nodes is a
property of the wire protocol and stays as it is; the work is in testing it against each of them.

### Modbus - beyond v1

Contiguous registers read together, RTU, read on demand through a registration, a way back for a
write's outcome. [The Modbus bridge](modbus-bridge.md).

### A registration that names a bridge

A value the broker does not hold, fetched from a device when read (a battery level that polling would
drain): a registration whose endpoint is a bridge scheme, served by the bridge.

## The API and the specification

### Subordinate subscriptions on registration change

§ 10.5.2.4 handles creation and deletion of a registration, not `PATCH`.

### Advertising the user @context and the core version

The `;v1.9` parameter on the core context's Link header, and § 13.4's `?core=`.

### Problem Details and error reporting - proposals to ETSI

Machine-readable errors and per-item outcomes that NGSI-LD does not define yet - taken to the ETSI
TC DATA face-to-face in Athens; not implemented. The drafts are not public.

### Our own string collation, replacing ICU

§ 7.6.2.1 makes ICU "root" collation the default order for `orderBy` on strings, and libicu costs
three shared libraries and 39.2 MiB - `libicudata` alone is 31.6 MiB, nine times the broker, for a
table. An MVP of root collation in corNgsild: UTF-8 to code points, a primary/secondary/tertiary
weight table for the Latin ranges real deployments use, the category order (punctuation < digits <
letters) the current ASCII approximation gets wrong; locale tailorings added on demand, one at a
time. ICU stays behind `COR_FEATURE_ICU_COLLATION=ON`. `orderby_collation_locale.test` is the
discriminator the MVP has to turn green without ICU.

### Array reduction in corJsonld

One JSON-LD normalisation applied once at the input boundary rather than at each call site.

### One MIME-type handling

The media types parsed, negotiated and rendered in one place instead of several.

## Security

### Authorisation inside the broker

NGSI-LD defines no authentication or authorisation. An optional layer inside coraine decides on the
request it has already parsed: per entity in a batch, for entity types after `@context` expansion,
by tenant - "may only read type X" becomes a condition on the query, not a filter on the response.
ODRL policies, as in the FIWARE Data Space Connector; the identity is the verifier's token for a
verifiable credential. [Authorization in the broker](authorization.md).

### An NGSI-LD plugin for APISIX

A policy enforcement point that understands NGSI-LD, as a plugin for [APISIX](https://apisix.apache.org/)
- the gateway that already is the enforcement point of every FIWARE Data Space Connector deployment.
It decides on the NGSI-LD request itself: the operation, the entity type (from the request, the entity
id, or the query), the attributes, the tenant - with the client's `@context`, as the broker would read
it. It sits in front of **any** NGSI-LD broker - Scorpio, Stellio, Orion-LD, coraine - and belongs in
APISIX's plugin hub; no such plugin exists today. The same decision rules as the authorisation inside
the broker, for a deployment that keeps them at the gateway.

## Operations and availability

### Migrating from Orion-LD

A deployment of Orion-LD moves to coraine with its data: entities, subscriptions, registrations.
First a converter - an executable that reads an Orion-LD database and writes coraine's (MongoDB for
`mongoc`, the files of a persistent corDB). Then, the general way: ETSI TC DATA has agreed that a
broker should be able to save its data in a **neutral format** (possibly a set of CSV files; nothing
about it is decided yet). With that format, any broker's export is coraine's import, and the
converter becomes "export, import". To be taken further at the Athens face-to-face, October 2026.

### haaux

High-availability cache synchronisation without a shared database: brokers register with each other
at startup, keep the connection, and sync subscriptions, registrations and contexts interrupt-driven
in single-digit milliseconds. No polling. [High availability](high-availability.md).

### HA peer push over a shared database

Where the brokers share a database, the one that changes a subscription, registration or @context
pushes the change to the others itself - over cor://, in parallel with nothing in between - and
answers the client once they have acknowledged it. The change stream (~50 ms: majority commit, then a
re-read) stays as the safety net. The cluster's members live in a collection of the shared database
with a lease each, so a restarted broker finds and reconnects to the others. It closes the window in
which an entity update reaching another broker misses a notification. [Roadmap](roadmap.md), § 5.1.

### A memory budget and admission control

A broker that knows its memory budget (a container's limit) and refuses writes before the OOM killer
ends it - `--memoryLimit` is the start of it.

## Build, distribution and footprint

### Packages

A Debian repository and `apt-get install coraine`, with a `coraine-dev` that pulls the whole
dependency stack in one command. Today building from source is the only way to a machine that does
not run the container image.

### Finish conditional compilation

Per-feature `#ifdef`s so a deployment compiles only the NGSI-LD it uses. `REGISTRATIONS` and
`SUBSCRIPTIONS` compile out and the HTTP server is a build choice, but most declared feature flags do
not reach the code they name - [Building from source, in detail](building-details.md) says which.

### Release libraries, all of them

`make release` builds corRest, corJsonld and corNgsild as release; the libraries under them come in
as the last build, usually debug. Each needs an archive per flavour first (`doc/performance.md`).

### Embedded deployment

The broker adds 4.3 MiB to a machine, holds 17 MiB resident and answers 12 ms after `exec`; constrained
hardware is a build-configuration question, not a redesign - conditional compilation is what makes
that true.

### An ARM image, and a more diverse nightly

An ARM64 image; nightly builds with `unsigned char`, the sanitizers (ASan, UBSan), clang and musl -
each finds bugs the others hide.

## Quality

### Continuous ETSI conformance

The official test suite kept at 100 % as the specification evolves, test-side corrections fed
upstream.

### Broader performance regression coverage

More scenarios measured nightly and recorded, so a regression is noticed by CI, not by a user.

### `--connectionPoolSize` on one core

Why `corHttp` is slower than libmicrohttpd on one core, and whether the pool size is the cause -
[Performance](performance.md), "An open question".
