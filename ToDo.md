# coraine — ToDo

The backlog: things **not done yet**. Finished work lives in git history, not
here. The 2026-05-01 spec-gap audit that used to fill this file is closed — its
items are implemented, functested and pushed; the handful that were deferred *by
design* are restated below.

---

## 1. Service Execution

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

## 2. DDS

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

## 3. OPC UA

The same shape as DDS, for the other half of the factory floor. An OPC UA
transport plugin, addressed the same way (`opcua://`), so an NGSI-LD entity
and an OPC UA node are two views of one thing:

- OPC UA **variables** ↔ entity attributes, read and written through the broker;
- OPC UA **monitored items** ↔ NGSI-LD subscriptions, so a change on the server
  becomes a notification without polling;
- **methods** ↔ Service Execution (item 1) — an OPC UA method call is exactly
  the actuation-with-feedback shape.

Same dependency as DDS: it needs the protocol-plugin seam.

## 4. WebSockets

Notifications today require the consumer to *be* an HTTP server: the broker
POSTs to `endpoint.uri`. Anything behind NAT, a firewall or a browser cannot
receive one. A WebSocket binding turns that around — the consumer connects, the
broker pushes over the standing connection.

- Subscription delivery over an established WebSocket (`ws://` / `wss://`
  endpoints, including a handoff for a connection the broker already holds).
- Worth deciding at the same time whether the *API itself* is served over
  WebSockets, not only notifications — that is the interesting question for a UI
  that wants live state without polling.

## 5. haaux — the HA sync auxiliary

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
maintained**; **no polling, interrupt driven**; its own repository, same
libraries; REST endpoints `/subscriptions`, `/registrations`, `/contexts`,
`/admin`.

The seam is in place: `--high-availability <ip:port>` parses and refuses with *"the haaux
server, which is not implemented yet"* (`src/lib/ha/haInit.c`), and `HaEvent::apiP`
already carries the API representation so a payload-carrying channel needs no
database hop — `haEventApply()` refuses a non-NULL `apiP` on purpose, so the day
it arrives is a decision and not an accident. Blocked on the binary API (item 6).

## 6. Communication protocols as plugins

The fourth plugin category (see
[`doc/plugin-architecture.md`](doc/plugin-architecture.md)) is designed but not
built. REST/HTTP is compiled into the broker.

- **Make HTTP an IPC plugin.** Lift the REST layer out of the core behind a
  register symbol + driver struct, exactly like `dbRegister` / `troeRegister` /
  `apiRegister`. Nothing else can move until the seam exists.
- **Add a binary IPC plugin.** The ad-hoc binary wire protocol — TLV framing,
  no JSON parse on the hot path — running alongside or instead of REST.
- **Unblocks haaux** (item 5) and every bridged transport — DDS, OPC UA,
  WebSockets (items 2-4). None of them can start before this seam exists.

## 7. corDB — an NGSI-LD-aware database

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

## 8. Finish conditional compilation

`COR_FEATURE_*` flags exist in `CMakeLists.txt`, and that is nearly all that
exists. Selection works at build-tree level (`-DCOR_FEATURE_MONGOC=OFF` builds a
Mongo-free tree); the per-feature `#ifdef`s inside the C are next to
nonexistent, so switching off a core feature leaves its symbols referenced from
code that still compiles, and the link fails.

Goal: a broker compiled down to exactly the NGSI-LD a deployment actually uses —
no subscription engine on a read-only edge node, no geo, no tenants, no Mongo.
This is real work, feature by feature, not a switch to flip.

## 9. Load libmosquitto on demand

MQTT notifications are ~2 KB of broker code, but `libmosquitto` is `DT_NEEDED`:
every build links it, every process maps it (104 kB RSS) and every start-up
calls `mosquitto_lib_init()` — whether or not one MQTT notification is ever
sent. `dlopen` it on the first MQTT notification instead, the way plugins load.
That drops the hard link, the mapping and the init, and makes libmosquitto an
*optional* runtime dependency — which is what matters for a slim image.

---

## 10. Grow the performance suite

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

## 11. Unit tests for the libs, and CI to run them

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

## 12. A byte budget instead of a max entity count

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

---

## 13. EntityMaps nobody has to ask for

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

---

## 14. Functests that FOLLOW the pagination links

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

## 15. Persistence: one log, and history as a retention policy on it

corDB holds its entities in RAM. That is why it beats a broker on MongoDB by
the margins in `doc/performance.md`, and it is also why those margins are not a
like-for-like comparison: one of the two survives a restart. It is also the one
FIWARE requirement where the interesting configuration is the weaker answer -
see `doc/fiware-ge-checklist.md`.

### The shape

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

### What history stores is CONFIGURABLE, and that is not a detail

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

### So what does the TRoE boolean actually switch?

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

### Two TRoE requirements that change the shape (KZ, 2026-09-16)

**1. "Give me the Entity exactly as it was at time T."** Not the history list of
attributes with their timestamps - a *current-state Entity*, at an instant in
the past. Already decided at ETSI, so it is coming whether or not we plan for
it.

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

### The part that is actually hard

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

### No new libraries, still

Everything above needs `open`, `write`, `fdatasync`, `rename`, `ftruncate`.
Compression or a checksum would want `libz`/`libzstd`, and both are already in
a bare `ubuntu:26.04`. So the `corDB` + `corDB` deployment stays at **three
added libraries** after persistence, and both of those are optional features
(GEOS, mosquitto) rather than storage.

### mmap

Floated, and **not** on the critical path. Mapping the store file would make a
restart free, but it forces offsets instead of pointers and no allocation
during load, which is a different data structure rather than an edit to the
current one - a kjson-level change. The log-and-snapshot design above does not
foreclose it: if the record format is defined without pointer assumptions, a
mapped load can be added later as an optimisation rather than a rewrite.

### Order of work

1. **The record format.** It is the gate: `cor://`, the log and the snapshot
   all need it, and specifying it once is the difference between one serializer
   and two that diverge.
2. **Log + snapshot + recovery**, with group-commit `fsync`. This is what
   closes the FIWARE requirement and the "toy" objection.
3. **The selector**, with its documentation.
4. **The temporal index**, which is what makes in-process history answer
   queries rather than merely hold data.

### Still open

- **Retention policy shape** - a duration, a record count, a byte budget, or
  all three? A byte budget is the one an operator can actually reason about on
  a device, and it pairs with § 12.
- **Should the selector support `q`?** "Record only while speed > 50" is
  genuinely useful for anomaly history and genuinely awkward to evaluate per
  write. Deferred, not rejected.
- **One log per tenant, or one per broker?** Per tenant matches the lock and
  the store, and makes a tenant drop a file delete. Per broker is one fsync
  instead of N. Leaning per tenant.

## 16. corDB history is intrinsic; timescale stays a plugin

`--troe corDB` selects a TRoE plugin that keeps temporal history in the
process. Measured on eight shared cores, it costs **nothing**: 40 257 req/s
against 40 073 for `--troe none`, and 123 307 PATCH/s against 125 187. History
in PostgreSQL, on the same hardware, costs corDB **91% of its PATCH rate**
(11 099) and **94% of its batch rate**.

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

## 17. Two performance questions the 2026-09-16 numbers raised

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

---

## Smaller, still open

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

- `ngsildParse`: a **keyword enum** in the parsed node instead of a name
  pointer, so the hot path switches on an integer rather than `strcmp`. Worth
  1–3% CPU, but the real prize is the class of bugs it removes.
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
