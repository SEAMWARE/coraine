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

## 2. DDS

Speak DDS as a first-class transport, for the robotics and industrial side where
DDS is the bus and HTTP is the foreign body.

- **`local://<$NAME>` endpoints.** A subscription's `endpoint.uri` (and the
  registration equivalent) is an HTTP URL today, so every notification is an
  HTTP POST. `local://<name>` says: *do not go out over the network — hand this
  to the locally loaded transport called `<name>`*, e.g. `local://dds`. The
  notification never gets serialised into an HTTP request; it goes straight to
  the plugin, which publishes it on its own bus.
- Depends on the protocol-plugin seam (item 6): `local://` is only meaningful
  once transports are pluggable.
- The DDS side (client for DDS Services and Actions) exists from the ARISE work
  — this is about wiring it into the broker as a transport rather than a
  separate bridge process.

## 3. OPC UA

The same shape as DDS, for the other half of the factory floor. An OPC UA
transport plugin, addressed the same way (`local://opcua`), so an NGSI-LD entity
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
  endpoints, or a `local://` handoff for a connection the broker already holds).
- Worth deciding at the same time whether the *API itself* is served over
  WebSockets, not only notifications — that is the interesting question for a UI
  that wants live state without polling.

## 5. haaux — the HA sync auxiliary

The second channel for keeping several broker instances' caches in step. Today
`--ha mongo` uses the database's own change feed and measures **~50 ms**, with
two costs that cannot be tuned away: a change stream only delivers
majority-committed events, and the event carries no payload, so the receiver
re-reads the document before applying it. haaux has neither — the instance that
made the change pushes the change itself. **Single-digit milliseconds** is the
target, and it works for deployments with no shared database at all.

Settled already: brokers **register at startup and the connection is
maintained**; **no polling, interrupt driven**; its own repository, same
libraries; REST endpoints `/subscriptions`, `/registrations`, `/contexts`,
`/admin`.

The seam is in place: `--ha <ip:port>` parses and refuses with *"the haaux
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
- **Unblocks haaux** (item 5) and every `local://` transport — DDS, OPC UA,
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

**Libs**

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
