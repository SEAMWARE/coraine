# coraine Roadmap

This product is an Incubated FIWARE Generic Enabler. If you would like to learn
about the overall Roadmap of FIWARE, please check the section "Roadmap" on the
[FIWARE Catalogue](https://www.fiware.org/developers/catalogue/).

## Introduction

This section elaborates on proposed new features or tasks which are expected to be
added to the product in the foreseeable future. There should be no assumption of a
commitment to deliver these features on specific dates or in the order given. The
development team will be doing their best to follow the proposed dates and
priorities, but please bear in mind that plans to work on a given feature or task
may be revised. All information is provided as general guidelines only, and this
section may be revised to provide newer information at any time.

The detailed, day-to-day backlog — including what is deferred *by design* and why — is
[the second half of this page](#the-backlog-in-detail). It is kept current as work lands rather than
at release boundaries. Every idea, with what there is to say about it, is in [Ideas](ideas.md); the
lists below are one line each, linked to it.

## Available now

Recently built, and documented where it lives:

-   **DDS** - topics both ways, services, actions, through the DDS bridge ([DDS and ROS 2](dds.md)).
-   **MQTT** - device topics as Channels, and notifications to `mqtt://` ([The MQTT bridge](mqtt-bridge.md)).
-   **Modbus TCP** - registers as Channels ([The Modbus bridge](modbus-bridge.md)).
-   **`cor://`** - the binary protocol for GE-to-GE traffic: forwarding without a JSON parse, many
    requests in flight on one connection ([The cor format and cor://](cor-protocol.md)).
-   **Requests that wait run as coroutines** of the event loops, not on worker threads
    ([Coroutines](coroutines.md)).
-   **corDB on disk** - `--dbDir`: an append log synced every 100 ms and snapshots; the store
    survives a restart, still with no database server ([Installation](installation.md), "corDB on disk").
-   **Temporal history in corDB** - `--troe corDB`: the whole temporal API on the history corDB keeps
    itself, on disk with the store - no PostgreSQL, no second plugin.
-   **Service Execution** - "do this" in the API, not a write to an attribute: Service Descriptions,
    executions, synchronous and asynchronous, combined (GR CIM-055). `scopeQ` - a subscription's scope
    query, on the target entity's scope - selects the entities of a grouped execution (alone or with the
    other selectors) and narrows the entities a Service Registration offers its service on. A grouped
    execution refuses a member it does not know (400, naming it). A service name is unique within an
    entity: a registration (created or PATCHed) whose `serviceName` another registration of the tenant
    has, for entities both could select, is refused - 409 naming that registration. Two selectors are
    taken as disjoint only when that is certain: different types, different ids, an id the other's
    `idPattern` does not match, two anchored `idPattern`s with literal prefixes neither of which starts
    the other; `q`, `geoQ` and `scopeQ` do not separate. An entity that still has a name twice (an
    entity of two types, each with a registration of the name) answers an invocation of it with 409
    naming the registrations.
-   **Snapshots, on corDB too** - § 5.16 complete: writes and subscriptions on a Snapshot, on both
    stores.
-   **The WebSocket transport** - subscriptions and notifications over a WebSocket.
-   **Array reduction of entity data** - one-element arrays reduced at the input, by the @context.
-   **`make tune`** - the broker built for your workload: the profile trained on it, the allocator and
    the lock policy measured on it ([Extreme performance](extreme-performance.md)).
-   **Subscriptions at scale** - a write matched only against the subscriptions that can match it: +32 %
    with 1 000 subscriptions ([Performance](performance.md)).
-   **An NGSI-LD-aware JSON parser** - the core terms are an enum, not strings: a compare instead of a
    `strcmp` wherever a member is recognised.
-   **linux/arm64** - every image for amd64 and arm64 under one tag (arm64 without the DDS bridge), and
    the whole functional suite on native ARM runners every night.
-   **Migrating from Orion-LD** - `coraine-import`, a program of its own on the broker's libraries:
    entities, subscriptions, registrations and the temporal history, with their ids and system
    timestamps ([Migrating a database to coraine](migration.md)). Later, the ETSI neutral export format.
-   **Running on Kubernetes** - a memory budget that refuses work instead of being killed for it, a
    health port answering `/live` and `/ready` outside the HTTP queue, and a byte budget per query
    response - entity, temporal and distributed queries ([Kubernetes](kubernetes.md)).
-   **EntityMaps the broker creates itself** - a distributed query, and a local query paginated past its
    first page, freezes its set of matching entities on the first page and serves the pages from it
    (the links carry the map's id); an entity that no longer matches is left out of its page
    ([EntityMaps](installation.md#entitymaps)).

Bridges and Channels are coraine's own mechanism, not a standard: the concept goes to the ETSI TC DATA
face-to-face in Athens, 20–22 October 2026, and anything normative will realistically follow in 2027.
coraine implements its own objects now and adapts to whatever TC DATA settles on.

## In progress

## Short term

The following features are planned to be addressed in the short term and
incorporated in the next release of the product, in roughly this order:

-   **Service Execution, following GR CIM-055 as it settles** - the status names, the query and
    discovery parameters.
-   **Packages** - `apt-get install coraine`. ([more](ideas.md#packages))
-   **Finish conditional compilation** - every feature flag reaches the code it names. ([more](ideas.md#finish-conditional-compilation))
-   **Build GUI** - Qt/GTK application/webpage? to configure the build, incl cond.comp, pgo training, HW, etc => docker image
-   **An NGSI-LD plugin for APISIX** - an NGSI-LD-aware enforcement point in the gateway the FIWARE
    Data Space Connector already uses, in front of any NGSI-LD broker. ([more](ideas.md#an-ngsi-ld-plugin-for-apisix))
-   **Authorisation inside the broker** - per entity, per type after expansion, per tenant, per attribute; ODRL,
    verifiable credentials. ([more](ideas.md#authorisation-inside-the-broker))
-   **More build diversity, every night** - ASan + UBSan, clang, Alpine / musl (`-funsigned-char`, ARM's
    `char`, passed the whole suite).
-   **corDB persistence, cheaper writes** - a PATCH logs the attributes it touched, not the entity. ([more](ideas.md#cordb-persistence---cheaper-writes))
-   **corDB history: retention and a selector** - history kept for a time or a size, and recording only
    what it is told to (entities, attributes - subscription-shaped). ([more](ideas.md#cordb-history-retention-and-a-selector))
-   **More bridges** - Kafka, WebSockets, OPC UA on the Bridge/Channel seam. ([more](ideas.md#more-bridges))
-   **Subordinate subscriptions on registration change** - § 10.5.2.4 for `PATCH` too. ([more](ideas.md#subordinate-subscriptions-on-registration-change))

## Medium term

The following specific features are proposed to be addressed in the medium term,
typically within the subsequent release(s) generated in the next **9 months**:

-   **The IoT Agents as cor-agent plugins** - device protocols on the bridge contract; one binary or two tiers. ([more](ideas.md#the-iot-agents-as-cor-agent-plugins))
-   **Aligning Bridges and Channels with ETSI** - 2027, after the Athens face-to-face. ([more](ideas.md#aligning-bridges-and-channels-with-etsi))
-   **haaux** - high-availability cache sync without a shared database. ([more](ideas.md#haaux))
-   **HA peer push** - with a shared database, the broker that changes a subscription, registration or @context pushes the change to the other brokers itself (cor://) and answers the client once they have it - the window in which an update reaching another broker misses a notification shrinks from the change stream's ~50 ms to a LAN round trip. The cluster's members in a collection of the shared database. ([more](ideas.md#ha-peer-push-over-a-shared-database))
-   **Bridges and Channels over the API** - create, update, delete, persisted. ([more](ideas.md#bridges-and-channels-over-the-api))
-   **One compound geo index** - one index for every GeoProperty, both variants prototyped and measured.
-   **Hot and cold entity segments** - the entities written often apart from the rest (a design note first).
-   **Our own @context server** - the JSON-LD context hosting API (`/jsonldContexts`) as a small C server on our own libraries, native on every architecture; it replaces the Java `wistefan/context-server` (amd64 only) in the tests and CI, and can host the contexts a deployment publishes.

## Long term

The following are proposals regarding the longer-term evolution of the product.
Take into account that there is no commitment to deliver them in a specific
timeframe; they are provided so that potential contributors can see where the
product is heading and may wish to get involved.

-   **corDB standalone** - corDB as a server of its own beside the brokers, for high availability (the
    haaux role), with backups of current state and history. ([more](ideas.md#cordb-standalone---and-as-haaux))
-   **Our own string collation, replacing ICU** - root collation without 39 MiB of libicu. ([more](ideas.md#our-own-string-collation-replacing-icu))
-   **Array reduction for API objects** - subscriptions, registrations, ... - once the core context has
    `@set` for their arrays (entity data is done). ([more](ideas.md#array-reduction-in-corjsonld))
-   **Embedded deployment** - constrained hardware as a build configuration. ([more](ideas.md#embedded-deployment))
-   **NGSIv2 support** - the NGSIv2 API as an API plugin beside NGSI-LD, on the same stores: a way off Orion (NGSIv2) for deployments that cannot change their clients at once, and the other half of migrating from Orion together with the NGSIv2 reader for `coraine-import`. ([more](ideas.md#transports-as-plugins))
-   **A smaller DDS stack for the DDS bridge** - for where the bridge has to run: the DDS libraries are today the largest part of a coraine installation (about five times the broker), which is why the arm64 image and the Debian packages come without the DDS bridge, and a device agent on a small board needs far less. A DDS implementation in C, or the subset of the DDS wire protocol (RTPS) the bridge uses - discovery, reliable and best-effort readers and writers, CDR - on the same Bridge/Channel seam; interoperability with every DDS and ROS 2 node is unchanged, it is the wire protocol. ([more](ideas.md#a-smaller-dds-stack-for-the-dds-bridge))
-   **Continuous ETSI conformance** - 100 % kept as the specification evolves. ([more](ideas.md#continuous-etsi-conformance))
-   **Broader performance regression coverage** - more scenarios, nightly. ([more](ideas.md#broader-performance-regression-coverage))

Everything else - open questions, smaller improvements, proposals to ETSI - is in [Ideas](ideas.md).

---

## The backlog in detail

The things **not done yet**, with what there is to say about each. Finished work lives in git history,
not here.

### The short-term list, in the order of work (2026-10-08)

Done from it: `cor://`, coroutines (with multiplexing and PGO), corDB persistence, system timestamps once
per entity (corDB and mongoc), the attribute type in the node (`CorNode.kind`), Service Execution, array
reduction of entity data, tenants created only by a creation, Snapshots on corDB (writes and
subscriptions included), the WebSocket transport, `make tune`, the subscription candidate index. Open,
in order:

1. **Migrating from Orion-LD** - being built (below).
2. **The rest of the CorNode prefix layout** - after the attribute type in the node: smaller nodes, fewer
   cache misses walking trees; `perf stat` cache-misses on the GET scenarios before and after.
3. **A memory budget and admission control** - refuse work rather than be OOM-killed (§ 12 below is the
   byte budget for the entity count).
4. **linux/arm64** - underway: the images built and published for both architectures from the next
   merge on (deploy.yml, release.yml), the functional suite on GitHub's native ARM runners every night
   (the job is being brought up - its first run found a C23-only construct, corJsonld#28); then the
   other diversity builds: ASan + UBSan, clang, Alpine / musl. (`-funsigned-char`, ARM's `char`, passed
   the whole suite on 2026-10-08.)
5. **Security** - in the broker (`authorization.md` § 4), verifiable credentials, the FIWARE Data Space
   Connector drop-in.
6. **One compound geo index** - prototype both variants, measure.
7. **Hot and cold entity segments** - a design note.
8. **The batch-delete regression** - low priority (attribute updates matter far more).

Smaller, alongside:

- **Functest wall-clock** - from 2.43 s to 1.30 s a test is done; left: mongosh's ~0.27 s per drop (a
  small libmongoc tool instead), mongoc's ~0.15 s on close (the topology monitor's join).
- **A Grafana data source for NGSI-LD** - current state and temporal, queries; scoped after a
  conversation with its prospective user.
- **More bridges** - Kafka (one proposal across the brokers that have one), WebSockets.

### Short term (KZ, 2026-10-03)

#### Migrating from Orion-LD

An executable that converts an Orion-LD database to coraine's format - entities, subscriptions,
registrations; MongoDB for `mongoc` and the files of a persistent corDB. Needed before coraine can
be offered to Orion-LD's users as the way forward: they must be able to take their data with them.

**The longer road** (ETSI TC DATA): a broker saves its data to a **neutral format** - possibly a set
of CSV files, no details discussed yet. KZ pushes it in Athens (20-22 October 2026). Once it exists,
Orion-LD to coraine is export plus import, and so is any other broker. Design the converter so its
reading and writing halves can meet in that format. [Ideas](ideas.md#migrating-from-orion-ld).

#### The ETSI test suite: our fixes, current and upstream

`integration/all-fixes` (local, in `~/git/ngsi-ld-test-suite`, never pushed) is the corrected suite
coraine runs today - the upstream review queue moves at about three merges a month, so the branch is
what makes the fixes usable now; the merge requests make them official.

- **Merge `develop` into `integration/all-fixes`** - not done since !309/!311, about 30 conflicting
  files. Until then the branch tests against an older suite than everyone else.
- **Fold in the six MRs filed 2026-10-03** (!313-!318, the GeonicDB team's fixes for
  geolonia/ngsi-ld-test-suite-patches #2-#6, #14): !318 is on the branch, the four mock-server fixes
  and the 036 one are not.
- **`testsuite-doubts.md` up to date** - every entry says what the branch does about it and where its
  fix stands upstream (merged, in an open MR, ours only, spec question).
- **The rest upstream, as a few thematic MRs** (mock server, LdContextNotAvailable 503/504, DistOps
  fixtures, temporal, subscriptions ...) rather than one per doubt, and taken to the Athens
  face-to-face (20-22 October 2026) to agree who reviews which. The spec questions go there as
  issues. Pushing to the forge takes KZ's credentials.

#### corAlloc: the arena's sizes, and whether it must zero

A request's arena starts in an inline buffer (`CorRestState.kallocBuffer`, 8 KiB) and grows in chunks
(`allocSize`, 256 KiB - `corRestStateInit.c`); an allocation bigger than a chunk gets a block of its
own, freed with the rest at the end of the request. Both sizes were chosen, not measured.

- **The sweep**: chunk 16, 32, 64, 128, 256 KiB and 1 MiB; inline 4, 8, 16, 32 KiB. Retrieve, query
  `limit=20` and `limit=100`, PATCH, batch create and update - two cores, the same for every run:
  req/s, p99, instructions and cycles a request (`perf stat`), mallocs a request, RSS. Every result
  documented, the losers too; a default changes only if a size clearly wins. 256 KiB is above glibc's
  starting mmap threshold (128 KiB) - part of what to look at.
- **The zeroing**: corAlloc hands out zeroed memory (a memset per allocation; `calloc` for an
  oversized block) - insurance against a field nobody set, like the `next` pointer that once pointed
  nowhere. Its cost is at most ~2 % (all of memset under a create load, 2026-10-03). Before deciding,
  find what relies on it: a build that fills every hand-out with `0xFEEDC0DE` instead of zero (a
  forgotten pointer is then non-canonical and crashes at once, a forgotten length absurd), run the
  functional suite and valgrind on it - every report is a latent bug. Fix those; then measure, and keep
  or drop the zeroing on the numbers.
- The comment in `corRestStateInit.c` says corAlloc returns NULL for an allocation bigger than a
  chunk - stale: it gets a block of its own.

---

### 0. TRoE timescale: automatic chunking and compression

**Decided 2026-09-26 (KZ): do it, early.** Found by the A10 capacity benchmark:
with the plugin's current schema (7-day chunks, no compression) a day of 60 s
samples for 10 000 sensors is ~49 GB of row store, larger than RAM, and one
sensor's day is spread over thousands of pages - every multi-sensor query missed
0.3 s. The same data with TimescaleDB compression is 10x smaller (43 GB -> 4.3 GB)
and zone queries came in at ~200 ms P95. Today it only happens if a DBA types it.

The plugin's migrate step does it, **only when the TimescaleDB extension is
present** (the plugin also runs on plain postgres - nothing changes there):

- `--troeChunkInterval` (default 1 h): `set_chunk_time_interval`. Live inserts
  then always land in a small, uncompressed current chunk.
- compression settings: `segmentby = 'entity_id, attr_name'`,
  `orderby = 'observed_at DESC'` - one attribute of one Entity is contiguous.
- `--troeCompressAfter` (default 1 h): `add_compression_policy`. TimescaleDB's own
  background scheduler runs it - nothing in the broker, so several brokers on one
  database (HA) cannot race.
- `--troeCompressMinMB` (optional, KZ's idea): compress what is older than
  CompressAfter only once it adds up to more than X MB. Not built in (the policy
  is age-only) - a custom `add_job()` procedure, still inside the database. Look
  at `compress_chunk_time_interval` (merges small chunks while compressing) first;
  it may be the simpler answer to "don't compress lots of tiny chunks".

Costs to keep in view:
- late writes into compressed history are expensive: the UNIQUE index on
  `instance_id` makes an insert/modify/delete there decompress. Decompressing 23
  one-hour chunks (10k sensors) took 37 min; compressing them 106 s. The lag
  must be longer than data realistically arrives late.
- existing databases: changing the chunk interval affects new chunks only.

### 1. Service Execution

Actuation as a first-class citizen of the API. TS 104 175 Annex G describes
*suggested* actuation workflows — commands encoded as NGSI-LD data on the
actuator's entity, feedback flowing back the same way, with three levels of
confirmation (QoS 0: fire-and-forget, high rate, no feedback; QoS 1: delivery or
a payload in response; QoS 2: continuous status for long-running commands like a
door opening 10 %, 50 %, …). The annex itself says the conventions are not
enough for jobs that depend on each other, and puts *"a more evolved service
execution logic"* explicitly out of scope.

That evolved logic is what belongs here: structured building blocks for
actuation — commands, feedback, conditional and chained execution — implemented
in the broker rather than re-invented in every application.

**Starting soon — and it is the feature that brings `--wip` with it.** The annex is
mature enough in the spec/draft to build against, which is exactly the case the
gate under *Smaller, still open* was described for: implemented while the draft can
still move, so it must not change the default build under anybody. Build the flag
alongside the first piece of this, not before it and not after — its shape is
easier to get right with a real feature in hand, and this is that feature.

### 2. DDS

Speak DDS as a first-class transport, for the robotics and industrial side where
DDS is the bus and HTTP is the foreign body.

- **The endpoint's SCHEME picks the transport.** A subscription's `endpoint.uri`
  (and the registration equivalent) is an HTTP URL today, so every notification is
  an HTTP POST. `dds://<...>` says: *do not go out over HTTP — hand this to the
  loaded bridge for that protocol*. The notification never gets serialised into an
  HTTP request; it goes straight to the plugin, which publishes it on its own bus.
  The protocol names the endpoint (`dds://`, `opcua://`, `ws://`, `ngsild-bin://`);
  **`bridge` is the name of the seam, not of a scheme** — `local://` is not used,
  `local` already means "this broker only, do not forward" (`--localOnly`, `local=true`).
- Depends on the bridge seam (item 6): a non-HTTP scheme is only meaningful once
  transports are pluggable.
- The DDS side (client for DDS Services and Actions) exists from the ARISE work
  — this is about wiring it into the broker as a transport rather than a
  separate bridge process.

- **`-duc`, a default user context.** DDS people know nothing about JSON-LD, so
  the ARISE decision is that the mapping tool Engineering is building emits a
  **@context alongside the config file**, and the config file's short names
  (`attribute`, `entityType`) are expanded with it. coraine has no such option:
  `channelConfigLoad.c` expands with the core context, which is correct for a
  broker that has no default user context and wrong for a deployment that was
  given one.

  ⭐ **How it has to work, and it is one rule, not a list of places** (KZ,
  2026-09-22):

  > For ANY expansion, if there is no user-supplied context, the default user
  > context is used. With `-duc` set, the broker **NEVER** expands with only
  > the core context.

  Which includes every expansion that has **no incoming request behind it** -
  reading the bridge config file at startup, an arriving sample, a cache being
  rebuilt. There is no "user-supplied context" there, so the duc IS the user
  context.

  So the implementation is an accessor, not a sweep: ONE function answering
  "the context to expand with when the caller was given none" - duc if there is
  one, core otherwise - and every current `corLdCoreContext()` used as *the
  context* (rather than as *the core context specifically*) becomes a call to
  it. `ldContextResolve` already is that fallback for the request path; the
  startup and bridge paths have no equivalent today.

  ⚠️ **orion-ld gets this wrong and it is worth knowing why**: its substitution
  lives in ONE place, `mhdConnectionTreat.cpp:1406`, the HTTP request path.
  `ddsNotification` runs `orionldStateInit(NULL)` -> core context and never
  consults the duc, so with a duc configured the same short name expands to the
  duc's IRI over HTTP and to the @vocab IRI on its topic: **two attributes on
  one entity, and nothing says so.** That is the shape of bug the accessor
  above exists to prevent - the fallback is a property of expansion, not of a
  transport.

  ⚠️ **Whether it is MANDATORY for `--bridges dds`** is open. The config file
  already is - the broker refuses to start without it, deliberately. If the
  context is half of what the tool emits, the same argument says a DDS
  deployment without one is misconfigured rather than defaulted. KZ is
  consulting the ARISE project lead (2026-09-22).

  ⛔ One exception, and it needs stating because the rule above is absolute:
  the **catch-all's** attribute names (§ 3.6a of `doc/bridge-channels-details.md`) are
  deliberately NOT expanded at all - an unclaimed endpoint is a foreign
  system's name being quoted, not a term, and expanding it both validates it
  (`rt/chatter` fails the § 4.6.2 name grammar on the slash) and looks it up (a
  topic called `location` would land on the core GeoProperty term). It is put
  under `@vocab` directly - and *that* @vocab should become the duc's when
  there is one, since a user context may define its own.

- **Use the forwarding window: harvest what DDS answered while the forwards
  ran.** A write that is also forwarded to other brokers (distributed
  operations, via registrations) already waits for those forwards before it
  answers. DDS goes first anyway, so that wait is free time for the DDS side:
  whatever has come back by the time the last forward returns is written and
  answered with, instead of a 202 -
  - a **service** reply, even without `?ddsSync`;
  - an **action** goal that has already FINISHED, with its result. Some actions
    are fast, and some brokers or connections slow (KZ).

  Nothing lost, only gains (KZ 2026-09-27): no forwards, no change - and no
  worker cap either (`--ddsSyncWaitMax`), since the worker is held by the
  forward anyway. Open: the status for an action that finished inside the
  request - 202 says "accepted, not done", and it is done (200/204?).

### 3. OPC UA

The same shape as DDS, for the other half of the factory floor. An OPC UA
transport plugin, addressed the same way (`opcua://`), so an NGSI-LD entity
and an OPC UA node are two views of one thing:

- OPC UA **variables** ↔ entity attributes, read and written through the broker;
- OPC UA **monitored items** ↔ NGSI-LD subscriptions, so a change on the server
  becomes a notification without polling;
- **methods** ↔ Service Execution (item 1) — an OPC UA method call is exactly
  the actuation-with-feedback shape.

Same dependency as DDS: it needs the protocol-plugin seam.

### 4. WebSockets

Notifications today require the consumer to *be* an HTTP server: the broker
POSTs to `endpoint.uri`. Anything behind NAT, a firewall or a browser cannot
receive one. A WebSocket binding turns that around — the consumer connects, the
broker pushes over the standing connection.

- Subscription delivery over an established WebSocket (`ws://` / `wss://`
  endpoints, including a handoff for a connection the broker already holds).
- Worth deciding at the same time whether the *API itself* is served over
  WebSockets, not only notifications — that is the interesting question for a UI
  that wants live state without polling.

### 5. haaux — the HA sync auxiliary

The second channel for keeping several broker instances' caches in step.

**The first reason is not latency, it is coverage.** `--high-availability mongo`
works only when the current-state DB *is* mongo **and** that mongo is a **replica
set** — a change stream needs an oplog. Run the in-memory store and there is no
shared store to listen to at all: such a deployment has **no HA whatsoever** today.
That, far more than the milliseconds, is what haaux is for.

Latency comes second, and is still real: the mongo channel measures **~50 ms**, with
two costs that cannot be tuned away — a change stream only delivers
majority-committed events, and the event carries no payload, so the receiver
re-reads the document before applying it. haaux has neither: the instance that made
the change pushes the change itself. **Single-digit milliseconds** is the target.

Settled already: brokers **register at startup and the connection is
maintained**; **no polling, interrupt driven**; REST endpoints
`/subscriptions`, `/registrations`, `/contexts`, `/admin`.

⭐ **It is a helper executable OF THE BROKER, and its notes live here** (KZ,
2026-09-22) - not a project of its own with a repository and a README of its
own. There was a `~/git/haaux` holding one; it is superseded by this section.

#### 5.1 Peer push over a shared database (KZ, 2026-10-09)

haaux's push, without haaux, where there IS a shared database. The window it closes: a client
creates a subscription on broker A and then updates an entity; the load balancer sends the update to
broker B, which has not yet received the subscription through the change stream (~50 ms) - and the
notification is never sent.

- **Membership**: a collection of the shared database, outside every tenant (as the @contexts are),
  holds the cluster: each broker's endpoint and a lease it renews; a TTL index removes dead members.
  A starting broker reads it, connects to the others (cor://, persistent), then loads its caches - in
  that order, as `haInit` does with the change stream, so nothing falls between the two.
- **Push**: the broker that makes a change writes it to the database, pushes it to every peer in
  parallel, and answers the client when they have acknowledged it (one LAN round trip). After the
  201, every broker knows the subscription: an update that comes causally after it (the same client,
  or one told of the subscription) cannot miss it, wherever it lands. Between independent clients
  there is no before and after - an update a few microseconds earlier would not have matched
  either - so there the gain is the size of the window: the change stream's ~50 ms down to the push.
  A peer that does not acknowledge within a short timeout does not hold up the client.
- **The change stream stays**, as the safety net for a lost push (a peer restarting, a partition);
  a version per object (`modifiedAt`) makes applying a change twice harmless.
- **Scale**: every broker connects to every other - right for the 3-10 brokers of an HA deployment;
  beyond that, haaux as a hub.

It does not replace the standalone haaux: that one is for deployments with **no** shared database.

⏳ **Not this year, and not next in line either.** haaux is not needed until
**corDB is a real persisting database** (item 15), because until then the
deployment that has no HA at all is the in-memory one, and an in-memory store
losing its state on restart is the larger half of that problem. corDB
persistence should land this year; haaux is not a priority.

The seam is in place: `--high-availability <ip:port>` parses and refuses with *"the haaux
server, which is not implemented yet"* (`src/lib/ha/haInit.c`), and `HaEvent::apiP`
already carries the API representation so a payload-carrying channel needs no
database hop — `haEventApply()` refuses a non-NULL `apiP` on purpose, so the day
it arrives is a decision and not an accident. Blocked on the binary API (item 6).

#### The fourth cache: bridge Channels

The three caches haaux was designed around — subscriptions, registrations,
@contexts — are all the same kind of thing: state one instance learned that the
others need to know. The **Channel cache** is a fourth, it belongs here, and it
is **not** the same kind of thing.

⭐ **A Channel is a WRITER, not a fact.** A subscription on five instances means
five instances might notify, which is the known HA problem and is solved by
deciding who owns the trigger. A Channel on five instances means five instances
**publish onto a shared bus**. A PATCH landing on any one of them puts a sample
on the topic — and an actuator on the other end is not receiving a duplicate
notification, it is being commanded five times. Inbound is the harmless mirror:
all five hear the same sample and store the same value.

Two questions, and they are not one question:

- **The cache** — every instance must agree on what the Channels ARE. Ordinary
  HA sync, carried like the rest: the API representation of the Channel
  (bridge, endpoint, kind, direction, retention, tenant, entityId, entityType,
  attrName) in `HaEvent::apiP`, never a database model.
- ⭐ **The carrier** — which instance's bridge actually publishes. **Open.**
  "All of them" is wrong for an actuator; "one of them" needs an owner, a
  failover and a way to say so. Same shape as the notification-owner question
  and will probably want the same answer.

⚠️ **`--high-availability mongo` cannot carry Channels at all.** Its reach is
defined by what is IN mongo: it watches collections and re-reads documents. A
Channel is built at startup from the **config file**, held in RAM, and written
to no collection — so the change stream has nothing to see, and two instances
started from two different config files disagree silently and forever. That is
not a bug in the mongo channel; it is what a cache outside the database means.

While Channels stay config-file-only — a deliberate decision, and good enough
through ARISE — the answer is the blunt one: **give every instance the same
config file**, and treat a difference as a misconfiguration nothing reports.

### 6. Communication protocols as plugins

The fourth plugin category (see
[`doc/plugin-architecture.md`](plugin-architecture.md)) is designed but not
built. REST/HTTP is compiled into the broker.

- **Make HTTP an IPC plugin.** Lift the REST layer out of the core behind a
  register symbol + driver struct, exactly like `dbRegister` / `troeRegister` /
  `apiRegister`. Nothing else can move until the seam exists.
- **Add a binary IPC plugin.** The ad-hoc binary wire protocol — TLV framing,
  no JSON parse on the hot path — running alongside or instead of REST.
- **Unblocks haaux** (item 5) and every bridged transport — DDS, OPC UA,
  WebSockets (items 2-4). None of them can start before this seam exists.

### 7. corDB — an NGSI-LD-aware database

A store that knows what an entity is, instead of one the broker translates into.
Entities cached in RAM, persistence behind it. Ships as a current-state DB
plugin like any other.

- **Open design question, to settle before writing code:** the broker already
  keeps RAM caches for **subscriptions**, **registrations** and **@contexts**,
  each with its own sync path. Is there anything to gain by moving those three
  into corDB rather than caching entities *next to* them? Either corDB owns all
  cached NGSI-LD state and the three existing caches collapse into it, or it
  owns entities only and the caches stay where they are. Decide deliberately —
  the answer shapes the whole design.

### 8. Finish conditional compilation

`COR_FEATURE_*` flags exist in `CMakeLists.txt`, and that is nearly all that
exists. Selection works at build-tree level (`-DCOR_FEATURE_MONGOC=OFF` builds a
Mongo-free tree); the per-feature `#ifdef`s inside the C are next to
nonexistent, so switching off a core feature leaves its symbols referenced from
code that still compiles, and the link fails.

Goal: a broker compiled down to exactly the NGSI-LD a deployment actually uses —
no subscription engine on a read-only edge node, no geo, no tenants, no Mongo.
This is real work, feature by feature, not a switch to flip.

### 9. Load libmosquitto on demand

MQTT notifications are ~2 KB of broker code, but `libmosquitto` is `DT_NEEDED`:
every build links it, every process maps it (104 kB RSS) and every start-up
calls `mosquitto_lib_init()` — whether or not one MQTT notification is ever
sent. `dlopen` it on the first MQTT notification instead, the way plugins load.
That drops the hard link, the mapping and the init, and makes libmosquitto an
*optional* runtime dependency — which is what matters for a slim image.

---

### 10. Grow the performance suite

`test/perf/perfRun.sh` measures one shape today: an entity query over 100 entities
at c50 and c200, plus retrieve-by-id. The nightly records the numbers on the
`perf-history` branch and warns when they drop. What it does not measure yet is
most of what makes a broker slow in production.

Planned, in order - each one ADDED rather than editing an existing scenario,
because a changed scenario silently invalidates every number recorded against it:

1. ✅ **Entity query over 100 entities** - what is measured today.
2. **The same, with NON-matching subscriptions loaded.** Every write walks the
   subscription cache whether or not anything matches; this is what that costs.
3. **The same, with MATCHING subscriptions, notified over HTTP.** Brings the
   notification path, the renderer and the HTTP client into the measurement.
4. Then: MQTT notifications, distributed operations against a second broker,
   temporal writes with TRoE enabled, geo-queries, and batch operations.

The thresholds are wide on purpose - warn at -20%, fail at -50% against the median
of the last five runs - because a shared runner moves 20-30% by itself. A scenario
is worth adding only when its numbers are steady enough to mean something.

### 11. Unit tests for the libs, and CI to run them

No lib repo runs a test in CI. Not one — `corLibs` has a single workflow and it
builds the CI base image. So the only thing that has ever verified a Cor-Lib is
coraine's functional suite, downstream, through the broker.

That is not a gap in coverage so much as a gap in *when*: `corLibs/bootstrap.sh`
clones `corRest`, `corNgsild`, `corJsonld`, `corPlugin` and `corTest` from `main`
with no pin, deliberately — they move together. A lib change is therefore invisible
to CI until it is merged to `main`, and the coraine PR that needs it cannot build
before then. The first thing that can fail is the coraine PR, after the lib change
is already in. A lib regression is discovered by the consumer, one merge too late.

Where things stand:

- **k-libs** — `kjson`, `kalloc`, `kbase`, `ktrace`, `khash`, `kargs`,
  `kprom` each already ship a test binary (`kallocTest`, `kTest`, `khashTest`, …).
  They are written and they are not run by anything. This is the cheap half: a
  workflow per repo that builds the lib and runs its existing binary.
- **Cor-Libs** — `corNgsild`, `corJsonld` and `corPlugin` have no tests at all, and
  `corRest`'s `corRestTest` is a demo server (it starts on :8080 and serves a few
  endpoints), not an assertion. This is the half that has to be written.

`corNgsild` is where it matters most: it is the biggest of them, it is where the
NGSI-LD rules actually live, and it is pure enough to test directly — most of it is
tree in, tree out. `ldDistMerge` is the shape to start from. § 4.5.5.3 is a decision
table over (expiresAt, observedAt, modifiedAt) with a handful of branches; a unit
test enumerates them in milliseconds, where a functest needs three brokers, a
registration and twenty seconds to reach one of them. The same goes for `ldQParse`,
`ldScopeMatch`, `ldEntityMatch`, `ldCheckDateTime`, `ldIso8601Duration`,
`ldPickOmit`, `ldOrderSort` — all decidable from arguments alone.

The functests stay where they are. They answer "does the broker behave", which is a
different question from "does this function compute the right answer", and they are
a bad instrument for the second one: the reason § 4.5.5.3 rule 3 could be dead on
the query path for as long as it was is that reaching one branch of one comparator
took a three-broker fixture nobody had written.

Order: the k-lib workflows first (the tests exist, so it is configuration), then
`corNgsild` unit tests, then the rest.

### 12. A byte budget instead of a max entity count

There is no ceiling on `limit` today. No cap in `ldUrlParams.c`, no constant
anywhere: `?limit=999999999` is accepted and attempted. Orion-LD had a hard 1000
with a default of 20, and that did not survive into this codebase.

Putting the 1000 back would restore a ceiling that never bounded the right
thing. **Entity count is the wrong unit.** A single entity with a large
JsonProperty, a long languageMap or a hundred attributes can outweigh a thousand
two-attribute ones, so a count-based cap lets a client ask for 1000 entities and
receive half a gigabyte, while refusing 1001 tiny ones. What actually needs
bounding is bytes - of the response, and of what the broker materialises to
build it.

Not a small change, because the budget has to be enforced where the bytes are
produced rather than checked afterwards:

- the DB plugins would need to stop mid-fetch when the budget is spent, which
  means rendering size is known during the scan rather than after it;
- the page that comes back is then a page the client did not ask for - shorter
  than `limit` - so `Content-Range` / the `next` link have to describe where it
  actually stopped, and a client must not read a short page as the last one;
- `limit` keeps its meaning as an upper bound on entities, with the budget as a
  second, independent bound. Whichever binds first ends the page. It is not a
  replacement for `limit` and must not be expressed through it - `limit` already
  means two things (a page size, and 0 for "empty array, just the count
  header"), which is one too many already.

Related, and the reason this moved up the list: the `orderBy` fix
(`DbQueryFilter::unpaged`) makes the broker fetch every match so it can order
before paginating. That is correct and it is genuinely unbounded - the one place
in the broker that will materialise an arbitrary number of entities on a single
request. Automatic EntityMap creation bounds *how often* that pass happens (once
per result set, not once per page); a byte budget is what would bound the pass
itself.

**Done** (`--maxResponseSize`; without it 1/16 of the memory budget, none outside a limit; [Response size](installation.md#response-size)): both
stores count each entity while fetching it (`DbQueryFilter.maxBytes`), a short page's `next` link starts
where it stopped, and the unpaged pass (`orderBy`, an EntityMap, split entities) answers 403
TooManyResults over the budget. Temporal queries too (`TroeQueryFilter.maxBytes`, timescale and
corDB): a short page, or one entity's instances cut to the K per attribute that fit with the temporal
pagination Link at `offsetN` + K, 403 when not even one instance per attribute fits. And the
forwarded part of a distributed query: each source's answer read up to the budget and no further
(`ldDistOpSendMultiMax`, corRest `corRestClientMultiMaxResponse`), the local and the forwarded pages
cut at one depth so that one `next` (`offsetN`) continues them all, 403 for the whole-set cases
(split entities, `orderBy`, an EntityMap) and for a source whose answer passes the budget.
~~**Open:** a split-entity distributed query forwards no `offset` / `limit` and applies no `offset`
itself, so it is refused over the budget rather than paged.~~ Paged through an automatic EntityMap
([§ 13](#13-entitymaps-nobody-has-to-ask-for)).

---

### 13. EntityMaps nobody has to ask for

An EntityMap is how a paginated query stays consistent - the ordered id list is
frozen once and the pages are served from it. In a distributed query there is no
other way to paginate correctly at all, and locally it is what stops an `orderBy`
query re-scanning and re-sorting the whole result set on every page. Orion-LD
creates one automatically; coraine does not, and the client has to know to ask.

Two things stand in the way, and the second blocks the first.

**13.1 - Nothing ever creates a map by itself.**
`corNgsild.entityMapCreate` is set in exactly two places: `?entityMap=true` in
`ldUrlParams.c:459`, and the explicit `POST /entityMaps` service routine
(`createEntityMap.c:38`). So a plain `GET /entities?type=X&orderBy=name&limit=20`
- or any distributed query - paginates without one. The broker should decide
that for itself: a map whenever the query is forwarded, and locally whenever
`orderBy` or a non-zero `offset` is in play. The client should only ever have to
follow the links it is given.

**13.2 - The pagination links drop the client off the snapshot.**
`ldPaginationLinkHeader` (`corNgsild/ldPagination.c`) rebuilds the query string
from the original URI params and skips exactly two of them,
`LD_PARAM_LIMIT | LD_PARAM_OFFSET`. Everything else is copied verbatim -
including `entityMap=true`. So the `next` link of a map-creating request says
`entityMap=true` as well, and following it creates a SECOND map: another full
scan, another freeze, and a page taken from a different snapshot than the one
before it. The link has to carry the id of the map that was just created, not
the request that created it.

That is a defect in the opt-in path as it stands today, not only an obstacle to
13.1 - and it is why 13.1 cannot simply be switched on. Automatic creation
without the link rewrite would create a fresh map on every single page.

**Confirmed on a running broker**, 2026-09-15. A `?entityMap=true&limit=2`
request answered:

```
NGSILD-EntityMap: /ngsi-ld/v1/entityMaps/urn:ngsi-ld:EntityMap:8d2d218788bf
Link: <...&orderBy=name&entityMap=true&limit=2&offset=2>;rel="next"
```

The map has an id and the `next` link does not use it. 13.2 wants a functest
that walks `next` from a map-creating request and asserts the map id never
changes - see 14.

**Done** ([EntityMaps](installation.md#entitymaps)): 13.2 - every link of a page that has a map names
the map (`entityMap=<id>`) and repeats the whole query that created it (§ 9.6: "the same parameters
as in the original request"); a page with a different selecting parameter is 400, and a page of an
expired or unknown map creates a new one from its parameters (§ 9.6) - 404 only without a selector. 13.1 - a `GET /entities`
whose first page has more after it gets a map of the broker's own: locally when the store holds more
than `limit` matches, distributed when the answer is more than a page (a second round asks the sources
for their EntityMaps). Not under `orderBy`: ordering by values is a Snapshot's. A map holds ids only
(`DbQueryFilter.idsOnly`) and the whole matching set - `?entityMap=true` froze `limit` + 1 before.
200 for an automatic map, 201 for a requested one; `NGSILD-EntityMap` on every answer a map took part
in, and honoured as a request header. `--entityMapMemory` (64 MiB) caps all maps, the least recently
used automatic ones giving way. A split-entity distributed query over the byte budget is paged through
a map instead of refused. `COR_FEATURE_AUTO_ENTITY_MAP` (ON). Tests: entitymap_auto_local,
entitymap_auto_distributed, entitymap_requested_pages.

---

### 14. Functests that FOLLOW the pagination links

Eleven tests assert a `rel="next"` / `rel="prev"` Link header. None of them
follows one: nothing in `test/funcTests/cases` extracts a link and re-requests
it.

That is how 13.2 stayed hidden. A `next` link carrying `entityMap=true` reads as
correct in an `--EXPECT--` block - it is the same parameter the client sent, and
the header is well-formed RFC 8288. The defect only appears when something
actually GETs the URL and notices it landed on a new snapshot. Asserting the
text of a link tests that we can print a link.

So: walk the links. From a query, follow `next` until it runs out, and assert the
concatenation of the pages is the result set - in order, no repeats, no gaps.
`orderby_limit_paginates_after_sort.test` step 10 does this for the plain
`orderBy` path and is the pattern to copy. The cases that still need it:

- the EntityMap path, which is where the bug is (this test will FAIL until 13.2
  is fixed - write it with the fix);
- `prev`, walked backwards to the first page;
- distributed queries, where the pages come from more than one source;
- a result set whose size is an exact multiple of `limit`, where the last page is
  full and the `next` link must not appear.

### 15. Persistence: one log, and history as a retention policy on it

corDB holds its entities in RAM. That is why it beats a broker on MongoDB by
the margins in `doc/performance.md`, and it is also why those margins are not a
like-for-like comparison: one of the two survives a restart. It is also the one
FIWARE requirement where the interesting configuration is the weaker answer -
see `doc/fiware-ge-checklist.md`.

#### The shape

**One append-only log is the whole mechanism.** Every mutation is appended with
its timestamps. That single artefact does three jobs, which is the reason to
build it this way rather than three:

1. **Durability** - replay it to recover.
2. **History (TRoE)** - it *is*, by construction, every value that was ever
   current. Nothing has to be written twice and nothing can disagree.
3. **`cor://`** - the same records, framed for a socket instead of a file.

The in-RAM tree is then not a second copy of the truth; it is a **materialised
view of the log's tail**, kept because reads want "the latest value of this
attribute" in O(1) and a time-ordered log cannot give them that. The
duplication buys the access pattern, which is what any index is for.

**Snapshots bound recovery.** Periodically write the tree out; replay only the
log after it. Under the per-tenant rwlock a snapshot can simply take the write
lock - at ~350 MiB per 100 000 entities that is sub-second, and correct, which
beats a copy-on-write scheme nobody can reason about.

**Group-commit `fsync`, on a timer.** Per-request `fsync` costs an order of
magnitude on writes; a ~100 ms timer costs almost nothing and loses at most the
last 100 ms on power loss - which is what MongoDB's journal does by default, so
it is parity rather than a compromise we invented. Configurable for anyone who
wants per-request durability and will pay for it.

#### What history stores is CONFIGURABLE, and that is not a detail

Recording everything is the wrong default for size and the right default for
surprise, so: **default everything, and let people narrow it.**

The selector is shaped like a subscription, because that is the shape users
already know and the semantics they already expect:

- **entity selection** - type, id, idPattern, as `entities` in a subscription
- **attribute selection** - a list, as `watchedAttributes`
- **include or exclude** - with "everything" as the default, the first thing
  anybody wants is "everything except this one enormous attribute", so an
  exclusion list is not a later refinement
- **CRUD at runtime**, and persisted itself so it survives a restart

⚠️ **Subscription semantics in the one way that matters: it affects the
future, not the past.** A selector is evaluated when the record is written, and
the record carries the verdict. Changing the selector does not retroactively
create history that was never recorded, and does not retroactively delete
history that was. Anything else makes "what is in my history" unanswerable.

This is implementation-defined - NGSI-LD says nothing about it - which means we
also owe it clear documentation, because nobody can read the specification to
find out how it behaves.

**The selector sits ABOVE the plugin seam.** It is a broker-level concept, not
a corDB one, so a deployment writing history to TimescaleDB gets the same size
saving from the same configuration. Putting it inside corDB would mean writing
it twice and having the two disagree.

#### So what does the TRoE boolean actually switch?

Not whether the log is written - durability needs it regardless. It switches
**retention**:

| | keeps |
|---|---|
| history off | only back to the last snapshot; compaction discards the rest |
| history on | history-eligible records for as long as the retention policy says |

Which is the honest answer to "a boolean to the db init function and after that
it just works": the log is unconditional, the selector decides what is
*eligible*, and the boolean plus a retention window decide how long eligible
records live. Compaction is where all three meet, and it is the only place that
needs to understand them.

#### Two TRoE requirements that change the shape (KZ, 2026-09-16)

**1. "Give me the Entity exactly as it was at time T."** Not the history list of
attributes with their timestamps - a *current-state Entity*, at an instant in
the past. Discussed at ETSI - **not in the specification yet** (checked 2026-10-03: no
branch of TS 104-175 or TS 104-176 has it), so it is **backlog**, implemented once it is
in the spec ([Ideas](ideas.md#the-entity-as-it-was-at-time-t)). The design below keeps
it cheap when it comes.

**2. The time axis is a parameter, not a constant.** `observedAt` (default),
`modifiedAt` and `createdAt` all have to work - they do today, via
`timeproperty`, and the suite covers all four including `deletedAt`. And
eventually users define their own TemporalProperties with names of their
choosing, and TRoE queries should work on those too.

Those two together mean **this is bi-temporal**, and that is not a detail:

| axis | what it means | is log order the same order? |
|---|---|---|
| `createdAt` / `modifiedAt` | **system time** - when the broker recorded it | **yes** |
| `observedAt` | **valid time** - when it was true in the world | **no** |

A device can deliver a late sample carrying an old `observedAt`. So:

- **Reconstruction by system time is nearly free in this design.** "The Entity as
  at T" is the log *prefix* up to T, applied over the newest snapshot at or
  before T. Log order is system-time order, so there is nothing to search.
- **Reconstruction by valid time is not a prefix.** It needs, per attribute, the
  record with the greatest `observedAt` ≤ T - which is exactly the
  `(entity, attribute, time)` index below, per axis. And the same question asked
  again later can legitimately give a different answer, because late data
  arrived. That has to be documented rather than discovered.

⭐ This is a strong argument FOR the log, not against it. Point-in-time
reconstruction is what an append log is naturally good at; on a row-per-change
schema it is "the latest value of each attribute before T", which is a window
function over the whole history table. If ETSI is adding this operation, the
architecture that makes it cheap is the one to have.

Consequences to build in from the first byte:

- **A record carries every timestamp it has** - `createdAt`, `modifiedAt`,
  `observedAt`, `deletedAt` - not one plus a convention. Cheap in the record,
  impossible to retrofit.
- **Snapshots serve double duty**: bounding recovery *and* bounding
  point-in-time reconstruction. So keep a *chain* of them, not just the newest,
  and make snapshot retention part of the retention policy.
- **Tombstones are part of reconstruction.** An attribute deleted before T must
  be absent from the Entity as at T, which means `deletedAt` records cannot be
  compacted away while the history that needs them is retained.
- **Which axes get indexed is a declaration, not a guess.** With user-named
  TemporalProperties the axis is a *name*, so there cannot be one fixed column.
  The history selector already says *what to record*; extend it to say *which
  time axes to index*. The user knows what they will query by, and an index
  nobody queries is pure cost.

⚠️ **And the honest limitation, which follows from the selector being KZ's own
requirement:** you cannot reconstruct what you did not record. If history was
narrowed to three attributes, "the Entity as at T" can only answer for those
three. That has to be visible in the response or the documentation - a
reconstructed Entity that silently omits attributes nobody chose to keep is
worse than an error.

#### The part that is actually hard

**Temporal queries need an index.** `timerel=between`, `lastN`, aggregation -
over a raw append log every one of those is a full scan. TimescaleDB supplies
that index today and an in-process history has to supply its own, keyed
`(entity, attribute, time)`. This is larger than the logging and it is the item
to be honest about in any estimate: the log is a week, the index is not.

**Size.** History is unbounded where current state is not, so this is where the
**NGSI-LD aware parser** pays for itself: the core terms are a closed set, so
`"type"`, `"value"`, `"observedAt"`, `"Property"`, `"Relationship"` become an
enum rather than a string - repeated once per historical sample rather than per
sample per character. The selector is the other half of the same answer.

**Deletion.** Tombstones in the log, and NGSI-LD's `deletedAt` semantics to
honour - so a record carries createdAt/modifiedAt/deletedAt *and* observedAt,
not just a value.

#### No new libraries, still

Everything above needs `open`, `write`, `fdatasync`, `rename`, `ftruncate`.
Compression or a checksum would want `libz`/`libzstd`, and both are already in
a bare `ubuntu:26.04`. So the `corDB` + `corDB` deployment stays at **three
added libraries** after persistence, and both of those are optional features
(GEOS, mosquitto) rather than storage.

#### mmap

Floated, and **not** on the critical path. Mapping the store file would make a
restart free, but it forces offsets instead of pointers and no allocation
during load, which is a different data structure rather than an edit to the
current one - a corTree-level change. The log-and-snapshot design above does not
foreclose it: if the record format is defined without pointer assumptions, a
mapped load can be added later as an optimisation rather than a rewrite.

#### Order of work

1. **The record format.** It is the gate: `cor://`, the log and the snapshot
   all need it, and specifying it once is the difference between one serializer
   and two that diverge.
2. **Log + snapshot + recovery**, with group-commit `fsync`. This is what
   closes the FIWARE requirement and the "toy" objection.
3. **The selector**, with its documentation.
4. **The temporal index**, which is what makes in-process history answer
   queries rather than merely hold data.

#### Still open

- **Retention policy shape** - a duration, a record count, a byte budget, or
  all three? A byte budget is the one an operator can actually reason about on
  a device, and it pairs with § 12.
- **Should the selector support `q`?** "Record only while speed > 50" is
  genuinely useful for anomaly history and genuinely awkward to evaluate per
  write. Deferred, not rejected.
- **One log per tenant, or one per broker?** Per tenant matches the lock and
  the store, and makes a tenant drop a file delete. Per broker is one fsync
  instead of N. Leaning per tenant.

### 16. corDB history is intrinsic; timescale stays a plugin

`--troe corDB` selects a TRoE plugin that keeps temporal history in the
process - **today a ring buffer of the last 256 events, for the functests that
assert a write produced its event; it answers no temporal query.** So the "costs
nothing" measured on 2026-09-16 (40 257 req/s against 40 073 for `--troe none`,
123 307 PATCH/s against 125 187) is the cost of that ring, not of history. History
in PostgreSQL, on the same hardware, costs corDB **91% of its PATCH rate**
(11 099) and **94% of its batch rate**.

**Decided 2026-10-03 (KZ): real history behind `--troe corDB`** - in the corDB store itself (the
broker takes the TRoE driver from corDB.so), recorded at corDB's write sites: current state
overwrites, history appends. 99.99 % of history is what leaks in from current state; the temporal
write endpoints (§ 5.6.11-16) are the correction path - nice to have. Phases: (1) history recorded
from current state, its own log segments, rebuilt at recovery, the retrieve - **done 2026-10-03**;
(2) reading it: query and retrieve with selectors, attrs, timerel / timeproperty, lastN / firstN,
pagination, count, datasetId - tested with current-state writes read back through the temporal GET,
restarts included; (3) q, geo, aggregation on history; (4) retention; (5) the temporal write
endpoints. Measured after each phase, against `--troe none` (and timescale).
**The selector** (what history records - a special
subscription consulted to keep or drop a write's history) is **not in the spec
yet: backlog**, like the Entity at time T.

So the in-process option is the interesting one, and it should not be reached
through the plugin mechanism at all. When the current-state store is corDB, its
history is the same log, the same lock and the same index - a boolean in
`corDbInit()`, not a separate `.so` with its own copy of the store.

The plugin was called `ramdb` until 2026-09-18, and the name is what made the
duplication easy to miss - a separate name makes it look like a separate
store. It never was one. That name made sense while corDB was `corRamDB` and
temporal was somebody else's problem; it stopped making sense when both halves
became the same data structure, so the plugin is now `corDB` on both axes.
Folding it into the boolean is the step after this one. See § 15 for what the
boolean actually switches, which is retention.

**The plugin seam itself stays**, and the symmetry is the point:

| | current state | history |
|---|---|---|
| in this process | `corDB` | `corDB`, via the boolean |
| an external server | `mongoc` plugin | `timescale` plugin |

Four combinations, one seam, and no option whose meaning changes depending on
the other axis. Somebody will legitimately want history in PostgreSQL so they
can run SQL and BI over it - that is a different choice, not a worse one - so
`timescale` remains exactly what `mongoc` is: the external-server answer on its
axis.

### 17. Two performance questions the 2026-09-16 numbers raised

Both are measured facts without explanations, which is the worst kind of
performance result to leave lying around.

**corHttp is now SLOWER than libmicrohttpd on one core, and
`--connectionPoolSize` is why - but the fix is not a new default.** 5 901 req/s
against 6 588; it used to be the other way round.

Not the loop count: `--httpLoops` resolves to 1 on one core, which is right,
and forcing 2 or 4 there is worse. Not the per-loop work queue either - the
worker pool is deliberately separate from the loop so the loop never blocks,
and that handoff predates the sharding.

It is the pool size, swept on one core (builtin): 2 -> 6 469 (p99 9.97 ms),
8 -> 6 905, 16 -> 5 082, 32 (default) -> 5 562, 64 -> 5 850. Something is there
at the low end, but 16 beating neither of its neighbours says the spread is
comparable to the effect, so it is not yet a number to act on.

⚠️ And the option CANNOT just be re-defaulted, because it means three things:

  corRestBackendStart(poolSize)     MHD: I/O thread count
                                    builtin: connection slots, poolSize * 16
  corRestWorkerPoolStart(poolSize)  both: request worker threads

Lowering it to 8 costs libmicrohttpd 15% - four times fewer I/O threads - and
for corHttp it moves workers and connection capacity together, so the sweep
cannot say which produced the gain. This is the "never express a new concept
through an existing mechanism" rule, arrived at from the performance side: the
work is to SPLIT the option - a worker count of its own, sized per CPU the way
--httpLoops is, and a connection capacity that stays a capacity - and only then
to choose defaults. A default change on top of the current knob is how a 23%
gain on one core becomes a 15% loss on another deployment.

**Orion-LD is faster than coraine + mongoc at batch updates.** 2 392 req/s
against 1 994 on eight shared cores, and 0.93x per broker core - two
independent measurements agreeing, so it is not noise. It is also the only
shape where coraine on MongoDB loses, which points at something specific in
`mongocEntityBulkUpdate` rather than at the mongoc layer generally. Worth
reading beside Orion-LD's batch path, which is public.

---

### 18. coraine as a device agent: small boards and MCUs

An agent next to the devices, not a central broker: stripped down considerably - no TRoE, no
registrations, no subscriptions (`COR_FEATURE_SUBSCRIPTIONS=0`, `COR_FEATURE_REGISTRATIONS=0`),
current state in RAM (ramDB). What it does need is its southbound protocol plugins: Zigbee,
UltraLight, MQTT and others (§6, the bridge family). After the ARM image.

**Two targets, two efforts:**

- **Small Linux boards** (Pi Zero class, Cortex-A, 64-512 MB): no OS work. The ARM image, the builtin
  HTTP server instead of libmicrohttpd, TLS optional, the byte budget (§12).
- **MCUs** (Cortex-M, ESP32 - no MMU, 256 KB-8 MB RAM): an existing RTOS, not an OS of our own -
  NuttX (the most POSIX: most of our C compiles as is), Zephyr (the most boards), FreeRTOS + lwIP (the
  smallest). They bring TCP, BSD sockets, TLS (mbedTLS) and a flash filesystem (littlefs). What changes
  in coraine: plugins linked statically (no `dlopen`), `poll` instead of `epoll`, coroutines on one
  thread, persistence (if any) on the flash filesystem.

**Speed is not the reason.** The kernel is ~28 % of the broker's CPU on a batch update (2026-10-04,
2 cores: TCP, epoll, syscalls); an OS of our own removes the syscall and switch part of that, not the
TCP work - 10-25 % on the same CPU. The reason is footprint: running where Linux cannot.

**The hard part is RAM, not the OS:** arena sizes, CorNode trees, @context expansion (a precompiled
core context). A full broker on a 256 KB part is not realistic - there, a small cor:// device client
talking to a broker is.

**Order of work:**

1. Measure: RSS idle and per entity, every optional feature off - that number says which MCU class is
   possible at all.
2. Boot on NuttX or Zephyr in QEMU (1-2 weeks).
3. A "coraine-micro" profile on a fat MCU - ESP32-S3 with 8 MB PSRAM, STM32H7 with 1 MB (1-2 months,
   most of it the memory diet).

---

### Smaller, still open

**Broker**

- Subordinate subscriptions are not re-evaluated when a **registration changes**
  (§ 10.5.2.4) — creation and deletion are handled, `PATCH` is not.
- Decide the foreground/daemon story. The broker **never daemonizes**, and
  `--foreground` / `-fg` is a dead flag: parsed into `fg` at
  `src/app/coraine/coraine.c:206`, read nowhere, and its help text claims a
  daemonizing default that does not exist. Either implement `--daemonize` and
  keep foreground the default, or drop the flag.
- PostGIS as a geo backend.
- **`--wip` — a gate for what is implemented ahead of the spec.** Not built yet, and
  deliberately: the option has nothing to gate today, and its shape is easier to get
  right with a real feature in hand than invented in advance. Add it with the first
  draft-ahead feature, not before — which is **Service Execution** (§ 1), starting soon.

  Orion-LD has it — `-wip entityMaps,distSubs,dds,ws`, hidden, comma-separated — and
  two things there are worth NOT copying, because they share one cause: the valid set
  is written down three times.
    - The error text says `allowed: 'entityMaps', 'distSubs', 'ws'` while the code also
      accepts `dds`. The help lies about one of its own values and nothing can catch it.
    - `char* wipV[4]` with `kStringSplit(..., 4)` caps the list at four. There are
      exactly four features, so it works — and the fifth one silently disappears from
      the middle of a comma list, with no error.

  So drive all three from one table:

      typedef struct { const char* name; bool* flag; const char* what; } WipFeature;

  the parser walks it, the error message is generated from it, and the split is sized
  by it. Adding a feature is one line and cannot desynchronise the help.

  kargs supplies the rest: `KaString` with sort `KaHid` — hidden, always optional,
  the equivalent of Orion-LD's `PaHid`. Give it `--wip` and `-wip` both, as every
  other option in coraine's table has.

  Why gate at all: the aim is to be current with the SPEC rather than its last
  release (see docker/QUAY.md), so features land while the draft can still move. A
  flag is what keeps "we implement the draft early" from meaning "the default build
  changes under you".

**Libs**

- **Array reduction — belongs in `corJsonld`, done once instead of five times.**
  It is a JSON-LD rule, and `corJsonld` is what owns @context knowledge: term
  definitions, `@type`, `@container`. Nothing above it can decide the question
  correctly, which is why the rule currently lives as five hand-written copies
  in the layers that should have been asking:
  `ldCheckAttribute.c:368` and `:630` (collapse on storage, § 5.2.6.4.6),
  `ldEntityToApi.c:174` (collapse on render), and the `@context` case written
  twice, in `postSubscriptions.c:260` and `postCsourceSubscriptions.c:172`.
  What is missing is one term-aware reducer in `corJsonld`, called at the INPUT
  boundary, so every layer above meets one shape.
  `corLdContextParse.c:278` is where it would pay for itself
  immediately: it wraps ANY array, one element included, as
  `isArray=true, url=NULL`, which is the whole reason the two subscription
  paths cannot use their own pass-through test and reach around the parsed
  context into the raw body instead.

  ⚠️ It is NOT a blanket transformation, and the render path already knows why
  (`ldEntityToApi.c:159`): a term whose `@type` is `@json` carries opaque JSON
  where an array's cardinality is meaningful - the JsonProperty `json` member,
  but ALSO any user-context term declared `@type: @json`, so the exclusion is a
  property of the term, not a fixed field name. `@container: @list` and
  `@container: @language` are excluded for the same reason, as is the temporal
  path, where array length feeds the aggregations. Any input-side reduction has
  to consult the active @context per term, exactly as the renderer does - which
  is the argument for putting it in `corJsonld` rather than repeating the
  judgement in `corNgsild` and in the broker.

  And it needs nothing from `corNgsild` to decide: every exception is stated in
  the @context itself - `@type: @json`, `@container: @list`,
  `@container: @language` - not in the API layer. `corJsonld` holds the term
  definitions, so it can answer alone. That makes the caller-side
  `collapseSingletonArrays` flag (`ldEntityToApi.c:171`) a symptom to remove
  rather than a pattern to copy: the temporal path should come out right because
  of what its terms are declared to be, not because a caller remembered to pass
  false.

- Distributed-op back-compat for `ngsildConformance` — the header is dropped on
  forwards (`corNgsild/ldDistOp.c:354`).
- Periodic-notification cache: parse the `geoQ` fields into
  `geoRel`/`geoGeometry`/`geoCoordinates`/`geoProperty`
  (`corNgsild/ldPernotCache.c:171`).

**Deferred by design** (listed so they are not re-raised as gaps)

- Multi-source **temporal** pagination (`Content-Range` across context sources)
  — waits for temporal distributed operations.
- `orderBy` across distributed operations — a k-way merge breaks under
  offset/limit with split entities; EntityMaps and Snapshots are the answer.
- EntityMap pagination optimisation: reuse the stored remote map id during
  page-fetch instead of a per-entity GET.
