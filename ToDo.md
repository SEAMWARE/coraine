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

- **k-libs** — `kjson`, `kalloc`, `kbase`, `klog`, `ktrace`, `khash`, `kargs`,
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
