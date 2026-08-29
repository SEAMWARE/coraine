# Bridges and Channels — design notes

Working design notes, not a user manual: they carry the reasoning, the wrong
turns and the open questions, because those are what stop a settled decision
from being re-argued. Draft 2026-05-22, revised 2026-05-25, 2026-08-26,
2026-08-27, 2026-08-29. **Not implemented.**

> **Revision 2026-05-25** — direction change. HTTP **stays inline** in
> the broker, not in a plugin. The earlier "HTTP refactor first as a
> no-op move into `http.so`" recipe is dropped. The bridge family is
> now specifically for **non-HTTP** transports of broker-to-CSR
> traffic. What the bridge work actually adds inside the broker is a
> transport-neutral request/response shape (`BridgeRequest` /
> `BridgeResponse`) and a single inbound entry point
> (`serveDistOp(BridgeRequest)`) that both the HTTP listener and any
> bridge plugin's inbound thread converge on. HTTP is then "the one
> inline implementor of the bridge interface" — it's just not
> packaged as a `.so`. See §1 and §9 for the updated framing.

> **Revision 2026-08-26** — the CSR-based mapping convention of §3 is
> **dropped**. Bridge endpoints are no longer Context Source
> Registrations. The mapping becomes two objects of its own — a
> **Bridge** (the transport instance) and a **Channel** (one
> foreign endpoint tied to one entity attribute) — because a
> registration makes a claim about the world that a bridge does not
> make. The endpoint **URI scheme convention is still shared** across
> registrations, subscriptions and bridges; only the object is not.
> §9.1 also widens: DDS services and actions are in scope after all,
> client-side only, and §10 changes from "defer them" to "adopt a
> provisional convention and quarantine it". See §3, §9.1, §10.

> **Revision 2026-08-27** — the model of §3 was derived from DDS, and DDS is
> unusually well-behaved. Tested against the other protocols a south bridge
> must carry, it holds for **addressed endpoints** (OPC-UA nodes, LWM2M
> resources, Modbus registers) and **breaks for bundle sources** (UltraLight,
> JSON, CSV, LoRaWAN, Sigfox, Kafka), where one arriving message sets many
> attributes on an entity it names itself. Consequences: a Channel gains a
> **codec**, device **provisioning turns out to be a shape of Channel** rather
> than a layer above it, and a Bridge may be a listener as well as a client.
> See §3.8. Also corrected: the Bridge resource is **not** read-only. The
> bridge *kind* is startup-fixed; the *instance* is a stored object with full
> CRUD, a 409 on a duplicate id, and an orphan case to answer — §3.5a. §3.5b
> settles how a Bridge names its plugin (short name, never a path in the
> payload), why the broker does not auto-create one from a plugin it finds,
> that both objects carry an `id` and a `type` like every other stored body,
> and that a **Channel names its Bridge explicitly** — the endpoint scheme
> stops being a selector as soon as two Bridges of a kind exist.

> **Revision 2026-08-29** — the object called a *Binding* is now a **Channel**.
> "Binding" is taken in TC DATA at document-title level (TS 104 176 *"NGSI-LD
> API Bindings"*, TS 104 243 *"MQTT Notification Binding"*), where it means how
> API operations are conveyed over a protocol; ours means how a value is carried
> to and from a foreign endpoint. The reasoning, and why *Map* is worse, is in
> §3. Also settled: the **three clocks** that the lifecycle question kept
> conflating (§3), a **`status`** on both objects so the degraded states are
> observable rather than silent (§3.3a), **no runtime plugin loading** (§3.5c),
> and the apparent contradiction between §4c's "fail loudly" and §3.5b's "boot
> anyway", which are different cases (§4c). Source layout answered too: **one
> library per bridge**, sibling to the other Cor-Libs, with the contract headers
> in a small `corBridge` of their own (§4a).

## 1. Goal

The broker must be able to forward distops and probe CSRs over
protocols other than HTTP — first TLV (wire format in the binary-IPC notes), then WS,
DDS, OPC-UA, etc. — *and* must be able to receive those same
operations on the inbound side when a peer broker sends them in a
non-HTTP transport. HTTP stays always-supported and inline (it's also
the NGSI-LD REST API surface; ripping it out would buy nothing).

Concretely, three pieces of work:

- **Transport-neutral types** inside the broker:
  `BridgeRequest` / `BridgeResponse` (semantic shape — op kind,
  tenant, entity payload, etc.). Every broker-to-CSR outbound call
  site builds a `BridgeRequest`, then calls `bridgeSend(scheme, req)
  → resp`. HTTP becomes the inline implementor for `http://` /
  `https://` schemes.
- **Transport-neutral inbound entry point**: `serveDistOp(req)`. The
  HTTP listener decodes a REST request into a `BridgeRequest` and
  calls `serveDistOp`. A TLV listener (when `tlv.so` is loaded) does
  the same with a TLV frame. The fan-out to actual service routines
  happens after `serveDistOp`, not before.
- **Bridge plugins** (the .so files) — one per non-HTTP transport,
  loaded on demand, implementing both outbound marshalling and an
  inbound listener thread that calls `serveDistOp`. First plugin:
  `tlv.so`.

This unifies three things that today live in different places:

- **Inter-broker over HTTP**: the existing distop forwarding path in
  `corNgsild/ldDistOp.c`. Both peers
  speak NGSI-LD; transport is HTTP. **Stays inline** — HTTP is the
  inline implementor of the new `bridgeSend` interface, not a plugin.
- **Inter-broker over binary** (new): same NGSI-LD peers, faster wire
  format. See the binary-IPC notes.
- **Inter-broker over WebSocket** (new): same NGSI-LD peers,
  persistent bidirectional channel.
- **Industrial bridges**: peer is *not* an NGSI-LD broker. DDS,
  OPC-UA, Modbus. Plugin translates between NGSI-LD and the foreign
  protocol. DDS is the first non-HTTP-peer concrete bridge, and on the
  order of 8 kLOC of translation work.

Multiple bridge plugins can be loaded **simultaneously** — unlike currentState
or TRoE (one active backend), the broker keeps a *table* of transports
keyed on URI scheme. HTTP occupies one slot in that table inline; every
other scheme is filled by a loaded `.so`.

## 2. Why a new family

The existing three plugin families are all "south/east-bound" — the
broker calls **into** the plugin.

| Family | Direction | Cardinality | Examples |
|---|---|---|---|
| currentState DB | broker → plugin | one active | mongoc, corDB |
| TRoE | broker → plugin | one active | timescale, ramdb, none |
| API | HTTP → plugin handler | many | admin |
| **bridge** (new) | **bidirectional** | **many, scheme-keyed (non-HTTP)** | tlv, ws, dds, opcua, … (HTTP is inline, not a plugin) |

A bridge plugin is the first one with **bidirectional** flow:

- **Outbound** (broker → plugin): a local entity write or a forwarded
  distop fires the bridge to send on the relevant transport (HTTP
  POST, TLV REQUEST frame, DDS topic publish, OPC-UA write, …).
- **Inbound** (plugin → broker): an external event arrives on a plugin-
  owned thread (HTTP response, TLV PUSH frame, DDS sample, OPC-UA
  notification) and must turn into a broker-side action — completing
  a distop future, updating an entity, firing a subscription.

This inverts the call direction. It requires a stable **broker API
surface** that plugins call back into — something the existing families
have never needed.

## 3. Endpoint-mapping convention: Bridge and Channel

> Supersedes the CSR-based convention of the 2026-05-25 revision. That
> version said "every bridge endpoint is a Context Source Registration
> with a protocol-specific endpoint URI". It isn't, and §3.1 is why.

Orion-LD configures DDS endpoints via a static JSON file listing
topics / services / actions, each mapped to
`(entityType, entityId, attribute)`. We are not adopting that as the
model either — but see §3.6, because we do have to keep reading that
file.

The model is two objects:

| Object | What it is | How many | Lifecycle |
|---|---|---|---|
| **Bridge** | A transport instance. For DDS: the participant — domain, QoS defaults, thread count, types directory. | Very few. | The *kind* is startup-fixed (`--bridges`); the *instance* is a stored object with full CRUD — §3.5a. |
| **Channel** | One foreign endpoint tied to one entity attribute: `dds://rt/pose` ↔ `(urn:ngsi-ld:robot:1, pose)`, plus direction, retention and kind. | More — one per topic per entity. | Created, patched and deleted at runtime. |

**Three things change on three different clocks, and conflating them is what
made the lifecycle hard to pin down:**

| | What it is | When it changes |
|---|---|---|
| The `.so` | Infrastructure. `--bridges dds,opcua` names which are loaded. | Startup. |
| The **Bridge** | Configuration. *A DDS participant on domain 7 with this QoS.* | Runtime, full CRUD. |
| The **Channel** | *This topic ↔ this attribute.* | Runtime, full CRUD. |

The relationship between the first two is one-to-many: two DDS participants on
different domains are two Bridges sharing one `dds.so`, both `"plugin": "dds"`.
An earlier draft made the Bridge read-only because the *kind* is startup-fixed
(§3.5a) — that was the first row's property applied to the second.

#### Why "Channel" and not "Binding"

The 2026-08-26 draft called this object a **Binding**. That name is taken,
at document-title level: TS 104 176 is *"NGSI-LD API Bindings"* and
TS 104 243 is *"NGSI-LD MQTT Notification Binding"*, and clause 5, 8 and 9
of TS 104 175 say "binding-specific" throughout. In the spec a binding is
*how API operations are conveyed over a protocol*. Ours is *how a value is
carried to and from a foreign endpoint*. Both involve a protocol, which is
what makes the collision confusing rather than harmlessly homonymous — a
reader meets "the DDS Binding" and reasonably expects a third API binding
beside HTTP and MQTT.

**Map** was considered and is worse. Clause 3 already defines two: *NGSI-LD
Entity Map* (Entity ids to registrations) and *NGSI-LD Map* (the JSON-LD
language map that LanguageProperty is built on). The second is a datatype
name used in normative prose. Worse, "map" names a static correspondence —
which is precisely what makes it sound like a registration, the thing §3.1
exists to distinguish it from. This object also declares direction and
retention: it says what *happens*, not only what corresponds to what.

**Channel** is unclaimed — two occurrences across TS 104 175/176/243, both
informal prose ("set up the communication channel", in the `notifierInfo`
row). It carries direction natively, it carries flow natively, and flow is
exactly the line against a registration: *a Channel carries the value, a
registration delegates it*. Bridge and Channel also hold together as a
pair; a bridge carries channels.

### 3.1 Why not a registration

A registration says: *another NGSI-LD context source holds this data;
forward NGSI-LD operations there.* A bridge says: *this attribute is
carried by a foreign transport; translate at the boundary.* The two
have nearly the same shape — an endpoint plus a set of entities and
attributes — and make different claims. That similarity is the trap,
not the argument: a new concept expressed through an existing
mechanism ends up sharing its code path, its counters and its status
word, and renaming afterwards does not separate them.

The decisive detail is where authority sits. **The broker retains the
value in both directions.** An arriving sample is stored. An incoming
`PATCH` on a bound attribute is *also* stored — and published on the
topic. That is not forwarding: the body is not handed to a peer that
then owns it. The broker keeps the value and puts a copy on a wire.

No registration mode describes that:

- `exclusive` — the source holds it, the broker must not.
- `redirect` — the client is sent elsewhere.
- `auxiliary` — the source complements what the broker has.
- `inclusive` — the closest, but it still means the broker **queries**
  the source and merges the answer. A DDS topic is never queried.

Nothing in the registration vocabulary says *the peer is a wire*.

### 3.1a Scope of that argument — pull is a registration

The paragraph above is about values that **flow**. It does not cover
values that are **fetched on demand**, and those exist: an attribute read
only when someone asks for it, because reading it costs something. The
canonical case is a device's battery level, where polling it is what
drains the thing you are measuring. Today that is expressible only
through a registration, and rightly so — "the value lives out there, ask
when asked" is precisely what a registration means, and `exclusive` is
precisely the mode for it.

So the two objects divide on **flow versus delegation**, not on
NGSI-LD-peer versus foreign-peer:

| The value… | Object | Direction of control |
|---|---|---|
| flows through the broker, in either direction | **Channel** | the transport pushes, or the broker pushes |
| lives at the peer and is fetched when read | **Registration** with a bridge-scheme endpoint | the broker pulls |

A registration whose `endpoint` names a bridge scheme reuses the
forwarding path, the CSR cache and the existing mode semantics, and the
plugin supplies the transport. Nothing new is needed in the registration
model — only a read method on the driver (§4), because an
`LdForwardRequest` is HTTP-shaped and a foreign peer cannot answer one.

This is not a coincidence, and it is not our invention either. It is the
same split the spec already documents. **TS 104 175, Annex G —
"Suggested actuation workflows"** — names two communication models for
what it calls a *Context Adapter* (§G.4.1):

- **Subscription/notification** (§G.4.2, implemented in §G.5). The
  adapter uses subscriptions to have command requests delivered to it
  and pushes results back, "acting as a Context Source as well as a
  Context Consumer". It holds no store of its own. **That is a
  Channel.**
- **Forwarding** (§G.4.3, implemented in §G.6). The adapter registers
  "I am responsible for command property X" in the Context Registry;
  the broker forwards the request to it. The adapter acts "as a Context
  Storage as well as a Context Producer" — it keeps the state, possibly
  on the device itself. **That is a registration.**

So both objects have a home in the existing spec vocabulary, and a
deployment picking one over the other is picking a documented workflow
rather than an implementation detail of ours.

⚠ Annex G is **informative**, and it is a *suggested workflow* rather
than a mechanism the API provides. That is the gap Service Execution
closes (§10) — and when it does, this section changes with it. What
should survive the change is the distinction itself: flow versus
delegation is a fact about where a value lives, not a convention.

One protocol can want both. OPC-UA monitored items push; OPC-UA reads
pull. LWM2M observes and reads. The model should let a deployment choose
per attribute rather than per protocol, and this does.

⚠ Lazy-over-notification — sending the peer a "go and read this" message
and waiting for it to write the value back — is what an agent must do
when it has committed to the push convention and has no registration. It
works, and it is strictly worse for a read: the client's `GET` waits on
a round trip through a notification and a subsequent write, which is what
registration forwarding already does in one hop. Do not model lazy that
way here because agents have to.

Two further consequences fall out. The §11 question about `endpoint`
having to be a reachable URI dissolves — `dds://rt/pose` is not a
registration's endpoint any more. And discovery stays honest: a
federation peer doing `GET /csourceRegistrations` gets context sources
it can use, not DDS topics it cannot.

### 3.2 What is shared: the URI scheme registry

The endpoint URI carries protocol and address, and that vocabulary is
common to registrations, subscriptions and bridges. One registry, read
the same way everywhere. The scheme list of the previous revision
stands unchanged (`ngsild-bin://`, `ws://`, `mqtt://`, `dds://`,
`opcua://`, `modbus://`, with `http(s)://` inline).

Share the URI convention. Do not share the object.

### 3.3 Channel fields

Beyond the endpoint and the entity/attribute selector, a Channel
carries two orthogonal axes that a registration has no vocabulary for:

- **direction** — `in` (the peer writes the attribute), `out` (a write
  reaches the peer), `both` (today's DDS topics, with loop protection).
- **retention** — `mirror`: store locally *and* put it on the wire, which
  is what Orion-LD does today; or `relay`: put it on the wire and keep
  nothing. Relay is not hypothetical — a 100 Hz pose stream that should
  feed subscriptions without persisting is exactly that, and so is an
  actuation command there is no reason to keep.
  There is deliberately no third `lazy` value: an attribute the broker
  does not hold and fetches on read is a registration, not a Channel
  (§3.1a).

Plus **kind** (`topic` / `service` / `action`, see §9.1), a **codec** where the
transport does not determine how to read the payload (§3.8), and any
per-channel transport overrides the plugin declares.

Defaults for anything created from the config file (§3.6): `both` and
`mirror`, which is Orion-LD's behaviour.

### 3.3a Status — the degraded states have to be observable

Both objects have a state in which they exist and do nothing: a Bridge whose
plugin was not loaded, and a Channel whose Bridge is unavailable (§3.5b). Those
states are the whole reason a stored object never vetoes a boot, and they are
useless if the only way to discover them is to notice that no data is arriving.

So both carry a **`status`**, read-only, computed rather than stored:

| Value | Meaning |
|---|---|
| `available` | the transport is up; a Channel is carrying |
| `unavailable` | Bridge: the plugin named by `plugin` is not loaded |
| `dormant` | Channel: it is well-formed, but its Bridge is not `available` |

plus a **`statusReason`** string where the value alone is not enough —
`"plugin 'dds' not loaded; not named on --bridges"` turns *"why is nothing
arriving from DDS?"* from a log-hunt into a `GET`.

Deliberately not an error field: an unavailable Bridge is not a broken one. The
configuration is valid and the infrastructure has not caught up with it, which
is a routine ordering fact (§3.5b) and is fixed by a restart with the plugin
named, not by editing the object.

### 3.4 Overlap with registrations

A Channel and a registration can both claim the same
`(entity, attribute)`. The rule is not "refuse any overlap" — it turns
on **retention**, and the predicate already exists.

`ldRegCacheLocalWriteConflict()` (`ldRegCache.c`) answers exactly the
question "may the broker write this attribute locally?". It walks the
registration cache filtering on

```c
if (itemP->mode != LdRegModeExclusive && itemP->mode != LdRegModeRedirect)
  continue;
```

— precisely the two modes that forbid the broker holding the data
itself — and matches on entity id, `idPattern`, type vector, scope and
attribute IRIs. Its overlap rules are already settled: an attrs-only
claim covers any entity; a claim with neither `propertyNames` nor
`relationshipNames` covers the whole entity; an attribute-scoped claim
conflicts only when the write carries one of its attributes.

A **mirroring** Channel is a standing declaration of intent to write
locally. So:

- Creating a mirroring Channel runs that check. Conflict → refuse.
- Creating an `exclusive` or `redirect` registration runs the mirror
  image against the channel set. Conflict → 409.
- A `relay` Channel writes nothing and conflicts with nothing.
- `inclusive` and `auxiliary` registrations never conflict with a
  Channel; they do not forbid a local write.

Runtime enforcement is already wired: the check is called from
`putEntityAttr`, `replaceEntity`, `patchEntity`, `postEntityBatchCreate`
and `postEntityBatchUpsert` — every local write path. Only the two
creation-time checks are new.

### 3.5 Why two objects rather than one

The alternative considered was a single Bridge object created without
its mapping and `PATCH`ed later to add the case-specific part. That is
the same split, collapsed into one object with an incomplete state that
then has to be validated, rejected on and reasoned about.

Split, the incomplete state disappears — a Bridge is complete when
created because it is a transport, and a Channel is complete when
created too. Everything else follows: the conflict check attaches to
Channels, which is where overlap lives (a DDS participant overlaps
nothing); deleting a Channel does not tear down a participant, while
deleting a Bridge cascades to its Channels; and `PATCH` still does the
job it was wanted for, on Channels.

### 3.5a Both objects are persisted, and both have real CRUD

An earlier draft made the Bridge resource read-only, on the grounds that
bridges are not created at runtime. That conflated two things. The bridge
*kind* is startup-fixed — a new one is a shared library and roughly a
month of work, and `--bridges` names which are loaded. A bridge
*instance* is not: nothing stops a second DDS bridge on a different
domain being created while the broker runs, and the plugin is perfectly
able to bring up another participant.

So a Bridge is an ordinary broker-held object, in the database beside
entities, subscriptions and registrations, with the lifecycle that
implies:

- **First startup**: somebody `POST`s the Bridge. It is stored.
- **Every startup after**: it is read from the database and its
  transport is brought up. Nobody posts anything.
- **Modification**: `PATCH`.
- **Re-`POST` of the same id**: **409**, "already exists" — exactly what
  `postCsourceRegistration` already answers for a duplicate registration
  id. Same rule, same status, no new concept for anyone to learn.

Two levels of conflict, and they belong in different places:

| Conflict | Detected by | Because |
|---|---|---|
| Same Bridge **id** | the broker | generic, and identical to every other stored object |
| Same **transport** — one participant per domain, one client id per MQTT broker | the **plugin** | only the driver knows whether a second instance of *its* transport is legitimate. Some allow it, some cannot. |

**The orphan case follows directly and needs an answer.** A Bridge in
the database whose plugin was not loaded — someone posts a `dds` bridge,
then restarts without `--bridges dds`. Do not refuse to start: mark the
Bridge unavailable, leave its Channels dormant, and say so in its status.
That is how the broker already treats a registration whose endpoint does
not answer, and a stored object should not be able to prevent a boot.

⚠ **`PATCH` on a Bridge is disruptive and should say so.** Changing a
domain, a QoS default or a types directory means tearing the transport
down and building it again, which drops and re-establishes every reader
and writer its Channels own. That is `bridgeUpdate` on the driver (§4).
Patching a *Channel* is cheap; patching a *Bridge* is not, and the
difference should be documented rather than discovered.

### 3.5b Which plugin, and what if it is not there

A Bridge is configuration; the `.so` is infrastructure. They are deployed by
different people at different times, which is what makes this feel awkward —
but it is an ordering fact, not a modelling problem. Three questions, and
the broker already answers all three for its other plugin families.

**How does a Bridge name its plugin?** By **short name** — `"plugin": "dds"`
— resolved exactly as `--database`, `--troe` and `--apiPlugins` resolve
theirs: against the plugin base directory, with a full path accepted only
when the argument contains a `/`.

⛔ **Not a path in the POST body.** A filesystem path submitted over the API
means the API can make the broker `dlopen` an arbitrary file — a remote-
code-execution primitive, and one that persists in the database across
restarts. It also welds a stored object to a filesystem layout: move the
installation and every Bridge is broken. The short name is indirection
through a directory the operator controls, which is the whole point of the
existing convention.

⛔ **And the broker does not auto-create a Bridge because it found a
plugin.** The reason generalises: *the presence of a plugin is a capability;
a Bridge is a decision, and a capability appearing must never enact a
decision.* Installing `dds.so` would otherwise make the broker join a DDS
domain by itself — on which domain, with which QoS, against which types
directory? Domain 0 is a guess that puts the broker on a network. It also
inverts §3.5a: there a stored object must not dictate the boot; here a
*file* would be creating stored state, and deleting the resulting Bridge
would only see it return on the next boot.

**So the answer is an error — at the right moment, and a different one at
each:**

| Moment | Plugin not loaded | Why |
|---|---|---|
| `POST` of a Bridge | **4xx, reject** | somebody is there to be told, and to fix it |
| Startup, loading from the DB | **mark unavailable, boot anyway** (§3.5a) | nobody is there to tell, and a stored object must not prevent a boot |

That asymmetry is deliberate: reject what can be rejected interactively,
degrade what cannot.

**"Must somebody really POST one at first startup?"** No — that is what
§3.6's loader is for. It exists to read the frozen configuration format, but
it is the right vehicle for any declarative "here are my bridges, no API
call needed" deployment: a small edge installation gets zero-configuration
startup from a file, not from a directory scan.

**Both objects carry an `id` and a `type`, as an NGSI-LD payload does.**
That is the shape everything stored in this broker already has — an Entity,
a Subscription, a ContextSourceRegistration all arrive as a body with those
two members — and there is no reason for these to look different:

```jsonc
{
  "id":     "urn:ngsi-ld:Bridge:dds-domain0",
  "type":   "Bridge",
  "plugin": "dds",                 // short name, resolved as §3.5b
  "config": { "domain": 0, "qos": { … }, "typesDirectory": "…" }
}

{
  "id":        "urn:ngsi-ld:Channel:robot1-pose",
  "type":      "Channel",
  "bridge":    "urn:ngsi-ld:Bridge:dds-domain0",
  "endpoint":  "dds://rt/pose",
  "entity":    { "id": "urn:ngsi-ld:robot:1", "type": "Robot" },
  "attribute": "pose",
  "direction": "both",
  "retention": "mirror"
}
```

The `id` is a URI and follows the same identifier rules as everything else,
which is what makes §3.5a's 409 precise: it is an id conflict, detected the
way the broker already detects one. It also gives the Channel something
stable to reference.

**Decided: they are JSON-LD.** The trajectory is standardisation — the
registration field of §3.5b is to be pursued in ETSI, and a Bridge put
alongside Service Execution would have to be a JSON-LD object with a real
`@context` — so `Bridge`, `Channel`, `plugin`, `direction` and `retention`
are terms in a namespace we mint and publish. Plain JSON would buy a little
work now and pay for it with a breaking migration later, in exactly the
deployments worth keeping.

⚠ **Some members must carry `@type: @json`, and the rule is about keys, not
values.** JSON-LD expands *keys*; a string value is left alone unless its
term is `@type: @id`. So `"endpoint": "dds://rt/pose"` needs nothing. The
hazard is every place a foreign name appears **as a key**:

- `config` — the `ddsmodule` half is passed verbatim and its keys are
  eProsima's vocabulary. Expanding `history-depth` into an IRI in our
  namespace is meaningless, and a future key outside § 4.6.2's name grammar
  would make a *valid configuration* fail as a *bad NGSI-LD name*
  (`ldVocabNameCheck` rejects it before `@vocab` ever applies).
- QoS maps and codec parameters — same shape, same argument.
- Anything mirroring the frozen file's structure, where a topic is a key:
  `topics: { "rt/pose": … }`. `rt/pose` contains `/`, which that grammar does
  not allow, so such a body would be rejected outright.

So: `@type: @json` on any member whose **keys** belong to somebody else. A
rule, not a judgement to be made field by field.

**A Channel names its Bridge.** Explicitly, by Bridge id, as a field of its
own. The endpoint scheme is not enough once two Bridges of the same kind
exist — two DDS participants on different domains both answer to `dds://` —
so the scheme becomes a consistency check rather than the selector: a
`mqtt://` address on a `dds` Bridge is a 4xx, not a lookup.

That gives referential integrity, and the rules follow the ones already
settled. A Channel referencing a Bridge that does not exist is rejected at
`POST` — somebody is there to be told (§3.5b). A Channel whose Bridge is
present but *unavailable*, because its plugin was not loaded, stays dormant
and does not prevent the boot (§3.5a). Deleting a Bridge cascades to its
Channels, which is no longer a convention but a consequence of the
reference.

⚠ **Registrations cannot do the same thing, and that asymmetry has to be
decided rather than discovered.** The pull case (§3.1a) uses an ordinary
NGSI-LD registration, whose members are fixed by the specification — there
is no room for a `bridge` field, and inventing one would be exactly the
overloading §3.1 argues against. Three ways out:

| Option | How | Cost |
|---|---|---|
| **`contextSourceInfo` entry** | `{"key": "bridge", "value": "urn:bridge:dds:1"}` | none — this is the member the specification provides for exactly this, and MQTT already carries `MQTT-QoS` and `MQTT-Version` through the equivalent `notifierInfo` |
| Encode in the endpoint | `dds://<bridge-id>/<address>` | overloads the URI authority, and the address grammar then differs between a Channel and a registration |
| Unique-or-error | resolve by scheme; 4xx when two Bridges match | no new syntax, but a second Bridge retroactively breaks working registrations |

**Take the first**, and keep the third as the convenience: resolve by scheme
when exactly one Bridge of that kind exists, require the entry when more
than one does. But take it knowing it is the wrong bucket, because that
matters for how it is implemented and for how it is undone.

⚠ **`contextSourceInfo` is the `receiverInfo` analogue, not the
`notifierInfo` one.** A Subscription has *two* of these: `notifierInfo`,
which configures our own end (`MQTT-QoS`, `MQTT-Version`), and
`receiverInfo`, which is headers to send to the receiver. A registration has
only `contextSourceInfo`, and it is the second kind — it becomes HTTP
headers on the outgoing forward. A Bridge reference is emphatically the
*first* kind: it says how our end behaves, and it is nobody else's business.
Registrations are simply missing the field, and the right fix is to add it
in ETSI rather than to keep borrowing the wrong one.

**Until then, the borrowed key must never reach the wire.** That is not new
machinery: `buildHeaders` in `ldDistOp.c` already consumes well-known
`contextSourceInfo` keys instead of forwarding them — `contentType`,
`accept`, `jsonldContext` are handled, and `Content-Length`, `Host` and
`NGSILD-Tenant` are banned outright. A Bridge reference joins that list. Six
precedents, one line.

**Decided: reserved keys take a sigil that cannot be an HTTP field-name** —
`@bridge`, not `bridge`. A field-name is a token and `@` is not one of its
permitted characters, so such a key could never have been a legitimate
header. The skip in `buildHeaders` becomes total and provable instead of a
deny-list that has to grow every time a well-known key is invented. The six
existing bare-named entries stay as they are; the sigil is the rule for new
ones.

**And the borrowing is temporary by intent.** A registration should have the
field a Subscription has — the `notifierInfo` counterpart, configuring our
own end rather than the receiver's headers. That is a specification change
to pursue, not a gap to live with, and `@bridge` in `contextSourceInfo` is
what holds the place until it lands.

⚠ And one limitation this creates for §3.6: the frozen configuration format
has no way to name a Bridge, so the loader creates one Bridge per plugin
from the `ddsmodule` half and binds everything to it. **A file-loaded
deployment therefore has exactly one Bridge per plugin.** That is not a
problem to solve — the format is frozen and one participant is what it
always described — but it should be written down before somebody expects a
second domain to appear from a file.

### 3.5c Decided: no runtime plugin loading

The remaining form of the question is *"the plugin was not there at startup —
may it be added while the broker runs?"*. **No**, and it is worth saying so
rather than leaving it unasked.

`dlopen` at runtime would be a one-way door. A bridge plugin spawns threads and
joins a transport in `init()` — a DDS participant starts discovering peers — and
`dlclose` on a library with live threads is undefined behaviour. So loading
would be possible and unloading would not: capabilities accumulating in a
process with no way back, and no way to test the way back either.

It would also make bridges the only plugin family that works this way.
`--database`, `--troe` and `--apiPlugins` are all startup-fixed, and a bridge is
the *least* casual of the four — it puts the broker on somebody's network.
Whatever triggered the load would reintroduce the argument already refused for
paths in a `POST` body (§3.5b): the decision to execute new code arriving over
the API.

⭐ **And it is not needed, because the degraded path is already the graceful
one.** A stored Bridge whose plugin is absent is `unavailable`, its Channels
`dormant`, and the boot proceeds (§3.3a, §3.5b). The operator adds the name to
`--bridges` and restarts; the Bridge comes up with its Channels intact, nothing
lost and nothing re-entered. A restart is a second on the edge installation this
would matter for. The whole of runtime loading buys the difference between that
and nothing.

### 3.6 The config-file loader

Orion-LD's DDS configuration file is not ours to change. `dds.ddsmodule`
is eProsima's and is passed verbatim to the enabler; `dds.ngsild` is
ours but is published and in use, so it is frozen in practice.

It maps onto the two objects one-to-one, which is a good sign the split
is right:

| File half | Object |
|---|---|
| `dds.ddsmodule` — domain, allowlist, QoS defaults, threads, logging | **Bridge** |
| `dds.ngsild.topics` / `.services` / `.actions` — `{ name → (entityType, entityId, attribute) }` | **Channels** |

The format is a **strict subset** of what the model expresses: three keys
per entry plus two globals (`typesDirectory`, `syncTimeoutMs`), and
nothing about direction, retention or lifecycle. That is the good case
for an interface that cannot change — the loader stays small, and
everything the file cannot say is free to be designed on the other side
of it.

So the loader's real work is **defaults**: everything the file does not
say must default to Orion-LD's behaviour. Which gives a testable
acceptance criterion:

> A Channel created from a config-file entry, with defaults, behaves
> identically to Orion-LD given the same file.

That is a functest, not a judgement call.

**Keep the loader.** An earlier framing had it as scaffolding to delete
when the project that motivated it ends. If the format is published and
in use, deleting it strands whoever runs that file. It is the
compatibility path for exactly those deployments: the file stays frozen
at today's expressiveness, the Channel API grows past it, and nothing
breaks for the file to stop being the only way in.

### 3.7 What it costs

- A collection, a cache, validation and lifecycle for Channels — the
  machinery a registration would have donated free. This is the real
  price of the separate concept. It is work, not overhead.
- Something CSR-subscription-shaped, if clients should be able to watch
  the channel set change.
- Two creation-time conflict checks (§3.4).
- A URI scheme registry that must be defined and documented before any
  bridge ships; once published, schemes are forever.
- The outbound and inbound refactors of §9 steps 1 and 2, unchanged —
  they are needed whatever the mapping model is.

Not on the list: lookup cost. Registration matching is ~1 665 lines
across 10 match functions resolving id, `idPattern`, type vector, scope,
property and relationship names and mode, over a linear scan of a set
that can hold one entry per entity id. A Channel lookup is an exact
match on one or two strings over a handful of entries. Only the outbound
path consults both; an arriving sample keys on the endpoint name
straight into the channel cache and never touches the registration
cache.

### 3.8 Beyond DDS — where the model holds, and where it does not

§3 was derived from DDS. Checked against the protocols a device-facing broker
would have to carry, DDS turns out to be the easy case: a topic carries one
typed sample, so one endpoint really is one attribute. Two families emerge.

| Family | Protocols | One message carries | Channel as defined |
|---|---|---|---|
| **Addressed endpoint** | DDS topic, OPC-UA node, LWM2M resource, Modbus register | one value | fits exactly |
| **Bundle source** | UltraLight / JSON over MQTT or HTTP, CSV, LoRaWAN, Sigfox, Kafka, ISOXML | many attributes, often many entities | does not fit |

**Why the second family breaks it.** UltraLight over MQTT is the most widely
deployed of them. Measures arrive on `/<protocol>/<api-key>/<device-id>/attrs`
with a payload like `t|15|h|40`, and `#` separates groups that each become a
separate NGSI request. So one subscription serves **many devices** — the
device is a path segment, not a topic — and each message sets **many
attributes**.
Enumerating a Channel per attribute per device is impossible before the devices
speak and absurd afterwards: hundreds of objects describing one subscription.

LoRaWAN and Sigfox fail the same way and harder: the peer is a network server
rather than a device, the payload is opaque binary needing a per-device-type
decoder, and one uplink decodes to a whole set of attributes.

⭐ **The conclusion is about provisioning.** Device provisioning is not a
convenience layer over Channels — **it is the form a Channel takes for a
bundle source**:

- a **direct Channel** names an endpoint and an attribute;
- a **provisioned device** names a *source* — a topic pattern, a webhook
  path, a Kafka topic — plus a codec and a rule for extracting the entity id
  and the attribute names from what arrives.

Same concept one level up. It does not bind a value; it says how to *derive*
channels from traffic. Which explains what otherwise looks like a coincidence:
DDS integrations need no provisioning API and every device-protocol agent has
one. That was never a maturity difference. It is whether the endpoint addresses
a value or a stream of them.

Two smaller consequences:

- **Codec becomes a Channel field** (§3.3). The transport does not determine
  how to read the payload — an MQTT topic may carry UltraLight or JSON. DDS
  hid this because there the type comes from the wire.
- **A Bridge may be a listener, not only a client.** A DDS participant, an
  OPC-UA client and an MQTT subscriber reach out; an HTTP or CoAP endpoint that
  devices post to accepts connections. The object covers both; §8's lifecycle
  wording should stop assuming the first.

And a reminder of the boundary: the NGSI-LD-speaking transports — TLV,
WebSocket between brokers — are not Channels at all. They are registrations
and forwarding, as originally designed. The model has **three lanes**:
forwarding to a peer that speaks NGSI-LD, a registration for a value fetched
on demand (§3.1a), and a Channel for a value that flows.

## 4. Plugin contract

Same shape as the existing families: a single registration symbol
populates a vtable struct. Working name `BridgeDriver`.

```c
// corBridge/BridgeDriver.h
typedef struct BridgeDriver
{
  int           version;        // header-ABI version. Broker refuses
                                // to load if major doesn't match. New
                                // pointers below are always added at
                                // the end; never reorder or remove.
                                //   (The 2026-08-26 revision reshapes
                                //   this struct rather than appending.
                                //   Nothing is implemented yet, so there
                                //   is no ABI to break — the rule starts
                                //   applying at the first release.)
  const char*   alias;          // "dds"  / "opcua" / …
  const char*   pluginVersion;  // free-form plugin build version
  KArg*         args;           // plugin-specific CLI args

  // URI schemes this plugin handles. The dispatcher matches an endpoint
  // by scheme prefix — a CSR's endpoint for an NGSI-LD peer, a
  // Channel's for a foreign one. NULL-terminated.
  const char**  uriSchemes;

  // Lifecycle. init runs once at broker startup after kargsParse, and
  // brings the transport up — it receives the Bridge config (domain,
  // QoS, threads, typesDirectory for DDS). close runs at shutdown.
  // Plugin owns its own threads between.
  int         (*init)(LdBridgeConfig* config);
  void        (*close)(void);

  // Channel lifecycle hooks (revised 2026-08-26; were csrCreate/Update/
  // Delete). Called when a Channel owned by this plugin is created,
  // patched or deleted. The plugin opens / reconfigures / closes the
  // underlying protocol resource — DDS reader or writer, OPC-UA
  // subscription — according to the Channel's direction and kind.
  int         (*channelCreate)(LdChannel* channel);
  int         (*channelUpdate)(LdChannel* channel);  // before+after both in cache
  int         (*channelDelete)(LdChannel* channel);

  // CSR lifecycle hooks — the same three, for bridges whose peer speaks
  // NGSI-LD (tlv, ws) and is therefore addressed by registration rather
  // than by Channel. A plugin implements one pair or the other, not both.
  int         (*csrCreate)(LdRegCacheItem* csr);
  int         (*csrUpdate)(LdRegCacheItem* csr);
  int         (*csrDelete)(LdRegCacheItem* csr);

  // Outbound to an NGSI-LD peer — called by the distop dispatcher when
  // an operation routes to a CSR owned by this plugin. The same shape as
  // ldDistOpSend's HTTP path, minus the URL.
  int         (*sendOne)(LdRegCacheItem* csr, LdBridgeOp* op,
                         LdBridgeResult* result);

  // Pull, for a registration whose endpoint names this plugin's scheme
  // (§3.1a) — a lazy attribute read on demand. Not a Channel: the broker
  // holds nothing and asks. Separate from sendOne because sendOne carries
  // an LdForwardRequest, which is HTTP-shaped, and a foreign peer cannot
  // answer one.
  int         (*attributeRead)(LdRegCacheItem* csr, const char* entityId,
                               const char* attrName, KjNode** valueOut,
                               LdBridgeResult* result);

  // Outbound to a foreign peer — a bound attribute was written. The
  // plugin puts it on the wire; whether the broker also kept it is the
  // Channel's retention (§3.3) and is decided before this is called.
  int         (*channelWrite)(LdChannel* channel, KjNode* value,
                              LdBridgeResult* result);

  // Service and action invocation, foreign peers, broker as client
  // (§9.1, §10). invoke is request/reply under a timeout. goalSend
  // returns a goal handle; feedback, status and result arrive later via
  // the upcall surface of §5b, and goalCancel stops one.
  //   Provisional: the NGSI-LD representation these produce is the v0
  //   convention of §10 and is expected to change with Service Execution.
  int         (*invoke)(LdChannel* channel, KjNode* request,
                        int timeoutMs, LdBridgeResult* result);
  int         (*goalSend)(LdChannel* channel, KjNode* goal,
                          LdGoalHandle* handleOut);
  int         (*goalCancel)(LdChannel* channel, LdGoalHandle handle);
} BridgeDriver;

typedef void (*BridgeRegisterFunc)(BridgeDriver* driverP);
// Plugin must export:  void bridgeRegister(BridgeDriver* driverP)
```

The `BridgeDriver.h` and `BridgeBroker.h` headers themselves live in a
**`corBridge` Cor-Lib**, not in the broker repo. Plugins link against the
headers there; the broker provides the implementations of the upcall
functions declared in `BridgeBroker.h`. See § 4a for the source-tree
layout decisions that follow from this.

### 4a. Source-tree layout

Bridge plugins do **not** live in the broker repo (unlike mongoc /
corDB today). One library each:

| Plugin | External lib | Location |
|---|---|---|
| (`http`) | — | *inline in the broker — not a plugin* |
| `tlv` | none (custom codec) | `~/git/corTlvBridge/` |
| `ws` | none (WS frame parser in-tree) | `~/git/corWsBridge/` |
| `mqtt` | mosquitto (already a broker dep for notifications) | `~/git/corMqttBridge/` |
| `dds` | Fast-DDS + FIWARE-DDS-Enabler (heavy) | `~/git/corDdsBridge/` |
| `opcua` | open62541 or similar (heavy) | `~/git/corOpcuaBridge/` |
| `modbus` | libmodbus or custom | `~/git/corModbusBridge/` |

External-dependency weight no longer decides *where* a plugin lives, only what
it drags in. Somebody building the stack should not need Fast-DDS installed
just to get a broker, and with one repo each that falls out for free: a heavy
plugin is opt-in because nobody clones it.

**Every bridge is its own library, sibling to the other Cor-Libs.**
`~/git/corTlvBridge/`, `~/git/corDdsBridge/` and so on, beside `corRest`,
`corJsonld`, `corPlugin` and `corNgsild`. `corLibs` is the umbrella — it drives
the siblings and holds no library of its own — so it is not a home for them,
and the old "thin ones stay as sub-libs in the umbrella" rule has nothing left
to attach to. Dependency weight stops deciding *where* a plugin lives and goes
back to deciding only what it drags in: uniform layout, and a heavy plugin is
opt-in because nobody clones it, not because it sits somewhere different.

**The contract is a Cor-Lib of its own — `corBridge`.** `BridgeDriver.h`,
`BridgeBroker.h` and `BridgeRequest.h` are what every bridge library compiles
against, and **`corPlugin` is the precedent for exactly this shape**: a small,
dependency-light library that exists only to serve the plugin system, sitting
in the libs tier beside the rest. `corBridge` is the same kind of thing. One
holds the mechanism for loading a `.so`, the other the contract a bridge `.so`
must satisfy.

The alternative — putting the headers in the broker repo, since the broker is
the sole implementor of `BridgeBroker.h` — costs two concrete things. The build
order is k-libs, then the Cor-Libs, then the broker last; contract headers in
the broker put six plugin repos in a tier *after* it, and point a library at an
application. And the broker publishes no headers today — `install` copies the
binary and its plugins to `/opt/seamware` — so it would grow a header-install
target and become a build-time dependency of six external repos.

The ownership argument for the broker repo — that a separate header can drift
from the code implementing it — is already answered by §4b: additive evolution,
same major number means it loads and works. That policy exists because the
plugins are external, and it covers the header for the same reason.

⚠ **`corBridge` is not part of `corPlugin`, however similar the two look.**
`corPlugin` is a generic `dlopen`/`dlsym` loader — name to path, symbol lookup,
handle tracking — and its README is explicit that it "knows nothing about" the
categories it loads. The database and TRoE families keep their contracts out of
it too. A bridge contract inside it would be the first thing to make the loader
know what it is loading. Same tier and same shape; different kind.

> Layout confirmed 2026-08-29. The one thing still hedged is **cor-agent**,
> which is expected to be a conditionally-compiled build of the broker in this
> repo rather than a repo of its own — but nothing here depends on that, because
> the contract's consumers are the plugin libraries, not a second broker.

**Naming**: `cor<Protocol>Bridge` for source (repo / dir), short protocol
name for the runtime artifact:

| Source | Built artifact |
|---|---|
| `~/git/corDdsBridge/` | `/opt/seamware/plugins/bridge/dds.so` |
| `~/git/corOpcuaBridge/` | `/opt/seamware/plugins/bridge/opcua.so` |

The `Bridge` suffix is the source convention only; the .so name is
the short scheme that matches the endpoint URI prefix.

### 4b. BridgeDriver header evolution

Once `BridgeDriver.h` and `BridgeBroker.h` live in `corBridge` and
get pulled into separate plugin repos, ABI breakage gets expensive
fast. The rule:

- **Evolve additively.** New function pointers always go at the end
  of the struct. Existing pointers never change signature.
- **New pointers are nullable.** The broker probes for `NULL` before
  calling — old plugins that pre-date the new slot keep working
  unchanged.
- **Removing a pointer is a major version bump.** The struct gains
  an explicit `int version` field at offset 0 so the broker can
  refuse to load a plugin compiled against an incompatible header.

This means we **don't pin plugin repos to a specific `corBridge` git tag**.
Plugins built against any `corBridge` version with the same major number
load and work — only optional features become unavailable for older
plugins.

Reconsider this policy when there are >3 external-dep plugin repos
and additive cruft starts to dominate the header.

### 4c. Discovery & CLI

> Revised 2026-08-26. Auto-discovery from the persisted regCache is
> dropped along with the CSR-based mapping of §3.

Install path: `/opt/seamware/plugins/bridge/<name>.so`.

**CLI**: `--bridges tlv,dds,opcua`, named explicitly, exactly as
`--database`, `--troe` and `--apiPlugins` already work. HTTP needs no
loading (always inline).

Named rather than inferred, for three reasons. A bridge brings up a
transport at `init()` — a DDS participant joins a domain and starts
discovering peers — and that is not something to do as a side effect of
a persisted object appearing in a cache. Startup then fails loudly when
a named plugin is missing, instead of silently not bridging until
someone notices. And a Channel can be created before its transport
exists only if the transport's existence is a startup decision; inferring
it from the channel set makes creation order load-bearing.

⚠ **This is not the same case as §3.5b's "boot anyway", though it reads like
it.** The two differ in who is asserting what, and when:

| What is missing | Where the claim came from | Startup |
|---|---|---|
| a plugin **named on `--bridges`** | the operator, in this invocation | **fail, loudly** |
| the plugin a **stored Bridge** names | a decision recorded at some earlier time | **`unavailable`, boot anyway** (§3.3a) |

A command line is an assertion being made right now, and an unsatisfiable one
should stop the broker before it half-starts. A stored object is a record from
the past, and the world may legitimately have moved on since. Reject what can
be rejected while somebody is watching; degrade what cannot.

The cost is one flag in deployments that would otherwise have been
implicit. Bridge *kinds* change roughly never — a new one is a shared

### HTTP stays inline

HTTP is the always-supported transport — it's also the NGSI-LD REST
API surface, so the broker can't ever ship without an HTTP path. The
existing distop forwarding code in `{sw,fw}Ngsild/ldDistOp.c` and the
direct-forwarding sites stays in-tree and becomes the implementation
behind `bridgeSend(scheme="http", req)`. What changes is the *shape*
of how it's called:

- Outbound call sites stop building HTTP requests directly; they
  build a `BridgeRequest` and call `bridgeSend`. The HTTP implementor
  reads `BridgeRequest` fields and constructs the HTTP request it
  used to build directly.
- Inbound HTTP requests (`httpRequestTreat`) stop fanning out to NGSI-LD
  service routines directly; they build a `BridgeRequest` and call
  `serveDistOp`, which does the fan-out.

This is **not** a packaging change for HTTP — no new `.so`, no
dlopen. It IS a structural change inside `{sw,fw}Ngsild` to push the
HTTP-specific marshalling out of the call sites and behind a
transport-neutral interface. Wire compatibility is unchanged; what
the HTTP listener sends and receives over the network is identical
to today. Only the boundary between "transport" and "semantic op" moves.

The first non-HTTP transport (`tlv.so`) then plugs in on the same
boundary without touching the HTTP path.

## 5. Broker API surface

Two distinct things live here:

1. **Transport-neutral request / response types** — used by every
   broker-to-CSR call site, regardless of which transport carries the
   call. HTTP listener and bridge plugins both build / consume these.
2. **Upcall surface for plugins** — the small stable header that
   plugin `.so` files link against to push samples into the broker
   from their own threads. This is the only sanctioned plugin → broker
   call surface.

### 5a. Transport-neutral request / response

```c
// corBridge/BridgeRequest.h
//
// A semantic representation of one NGSI-LD operation traveling
// between two NGSI-LD peers (or, for foreign-peer bridges, the
// translated semantic op). NO transport-specific fields — no HTTP
// status, no URL, no TLV correlation id, no MQTT topic. Those live
// in the transport-specific carriers that wrap a BridgeRequest at
// the boundary.
//

typedef enum BridgeOpKind
{
  BRIDGE_OP_CREATE_ENTITY,
  BRIDGE_OP_UPDATE_ENTITY,             // partial update (PATCH)
  BRIDGE_OP_REPLACE_ENTITY,            // full replace (PUT)
  BRIDGE_OP_APPEND_ATTRS,
  BRIDGE_OP_MERGE_ENTITY,
  BRIDGE_OP_DELETE_ENTITY,
  BRIDGE_OP_DELETE_ATTRS,
  BRIDGE_OP_RETRIEVE_ENTITY,
  BRIDGE_OP_QUERY_ENTITIES,
  BRIDGE_OP_BATCH_CREATE,
  BRIDGE_OP_BATCH_UPSERT,
  BRIDGE_OP_BATCH_UPDATE,
  BRIDGE_OP_BATCH_DELETE,
  BRIDGE_OP_BATCH_MERGE,
  BRIDGE_OP_TEMPORAL_CREATE,
  BRIDGE_OP_TEMPORAL_UPDATE,
  BRIDGE_OP_TEMPORAL_RETRIEVE,
  BRIDGE_OP_PROBE                      // CSR probe — info/sourceIdentity
  // (subscription / notification ops added later if MQTT/WS bridges
  //  extend scope to notifications — see §11)
} BridgeOpKind;

typedef struct BridgeRequest
{
  BridgeOpKind   op;
  Tenant*        tenant;
  const char*    entityId;             // single-entity ops; NULL otherwise
  const char*    attrName;             // attr-level ops; NULL otherwise
  KjNode*        body;                 // entity / fragment / batch array
  KjNode*        ldContext;            // expanded @context (parsed)
  const char*    via;                  // NGSILD-Via header value (loop-detection)
  const char*    path;                 // NGSILD-Path header value
  KjNode*        params;               // URL params as KjObject (limit, type, q, …)
  bool           sysAttrs;
  bool           local;                // ?local=true on retrieve / query
  // Future: orderBy, lang, geo* — extend as call sites need them
} BridgeRequest;

typedef struct BridgeResponse
{
  int            status;               // semantic outcome, mapped from
                                       // HTTP status when carried over HTTP,
                                       // from TLV result codes when carried
                                       // over TLV. Same numeric range as
                                       // HTTP for ease of code reuse.
  KjNode*        body;                 // ProblemDetails on error, entity/array
                                       // on success
  const char*    contentType;          // for retrieve; transport-neutral
                                       // media-type string
  // Headers semantically: Location (id on Create), Content-Range
  // (truncation), NGSILD-Via, NGSILD-Path. NOT raw HTTP headers.
  const char*    locationId;
  const char*    contentRange;
  const char*    viaOut;
  const char*    pathOut;
} BridgeResponse;
```

`bridgeSend(scheme, request) → response` is the outbound dispatch
function. Implemented in the broker; reads the scheme prefix, picks
the inline HTTP implementor for `http://` / `https://`, looks up the
loaded `BridgeDriver` for any other scheme, calls
`driver->sendOne(csr, request, response)`. See §6.

`serveDistOp(BridgeRequest*)` is the inbound dispatch function. Called
by the HTTP listener after it decodes a REST request, and by every
bridge plugin's inbound thread after it decodes its wire format. Fans
out to NGSI-LD service routines. See §7.

### 5b. Plugin upcall surface

```c
// corBridge/BridgeBroker.h
//
// Functions a bridge plugin may call on its own thread. Each call
// initialises the per-thread broker state (kalloc / FaAlloc, http
// allocator, tenant pointer) before running through the normal hooks,
// so subscriptions / TRoE / metrics fire as if the update had arrived
// over REST.
//
// Headers live in corBridge (NOT the broker repo) so that
// plugins built in separate repos (corDdsBridge, corOpcuaBridge, …)
// can link against the contract without depending on the broker
// source tree. The broker provides the implementations of these
// functions; the headers expose only the declarations.
//

// Per-thread allocator + tenant init. MUST be called once by every
// plugin-created thread before any other broker API call.
// The broker-wide rule for any new pthread: thread-locals are set up
// by the thread itself, never inherited.
int bridgeThreadInit(const char* pluginNameP);

// The transport-neutral inbound entry point. A plugin decodes its
// wire format into a BridgeRequest and calls this. Returns once the
// request has been served (or queued, for async transports).
int serveDistOp(BridgeRequest* req, BridgeResponse* resp);

// Convenience upcalls for foreign-peer bridges (DDS, OPC-UA, Modbus)
// that arrive with a single attribute sample rather than a full
// NGSI-LD operation. These build a BridgeRequest internally and
// call serveDistOp.

// Upsert an entity from a bridge sample. Replaces if exists, creates
// if not — the semantics an arriving sample needs, since a sample is a
// complete statement of the attribute's value rather than a patch.
int bridgeEntityUpsert(Tenant* tenant, KjNode* entity);

// Update one attribute on an existing entity (no-op if entity doesn't
// exist — caller usually pairs with bridgeEntityUpsert first).
int bridgeAttributeUpdate(Tenant* tenant, const char* entityId,
                          const char* attrName, KjNode* attrValue);
```

Implementation notes:

- Every call sets `corRest.bridgeSource = bridgeNameP` (or equivalent
  scoped flag) so the outbound dispatcher can skip the originating
  bridge. A sample that arrives on one transport and is stored must not
  be published straight back out of the transport it came from; the flag
  is what makes that decidable at the dispatcher.
- Threads created by plugins MUST call a `bridgeThreadInit(pluginP)`
  before any other broker API. Initialises kalloc, http allocator,
  tenant lookup. This is the broker-wide rule for any new pthread, not
  something the bridge family invents. The
  init function is wired through the broker API header.

## 6. Outbound flow

Every broker-to-CSR outbound call site goes through the same path:

```
call site                                  bridgeSend(scheme, req)
  │                                          │
  ├── build BridgeRequest from local data    ├── extract scheme prefix from
  ├── identify target CSR                    │   csr.endpoint URI
  ├── extract csr.endpoint scheme            ├── if scheme in {http, https}:
  └── call bridgeSend(scheme, req)  ────────►│       call inline HTTP impl
                                             │   else:
                                             │       look up BridgeDriver
                                             │       by scheme; call
                                             │       driver->sendOne(csr,
                                             │                       req,
                                             │                       resp)
                                             │
                                             └── return BridgeResponse
                                                 to call site
```

Call sites that need to update today:

- `ldDistOpSendMulti` in `{sw,fw}Ngsild/ldDistOp.c` — the central
  distop dispatcher. Stops building HTTP requests; builds a
  `BridgeRequest` per outbound leg and calls `bridgeSend`.
- Direct-forward sites that don't go through `ldDistOpSendMulti`:
  `postEntities.c`, `postEntityBatchCreate.c`,
  `postEntityBatchUpsert.c`, `postEntityBatchUpdate.c`,
  `postEntityBatchDelete.c`, `postEntityBatchMerge.c`, `patchEntity.c`,
  `postEntityAttrs.c`, `patchEntityAttrs.c`, `postEntityTemporal.c`,
  `postEntityTemporalAttrs.c`, etc.
- The CSR probe call in `ldRegCache.c` (the
  `<endpoint>/info/sourceIdentity` probe issued at CSR-register time
  and on refresh).

The inline HTTP implementor (`bridgeSendHttp`, conceptually) lives in
`{sw,fw}Ngsild` alongside the existing distop code — it's where the
HTTP-specific URL construction, header marshalling, `corRestClientRun`
invocation, and HTTP-status-to-BridgeResponse-status mapping live.
This is the only HTTP-specific code path; everything outbound above
it is transport-neutral.

The bridge plugins implement the same contract via their
`BridgeDriver::sendOne` vtable slot — they translate a `BridgeRequest`
into their wire format, send it, and fill in `BridgeResponse` on the
other side.

## 7. Inbound flow

Symmetric with §6. Today, the inbound HTTP listener
(`httpRequestTreat` and its callees) routes a parsed REST request
straight to one of the NGSI-LD service routines (`postEntities`,
`patchEntity`, `getEntities`, …) via the URL+method dispatcher in
`ldRestService.c` / equivalent. That coupling has to be unwound so
the same set of service routines can be reached from a non-HTTP
inbound path.

```
HTTP listener (existing)                 TLV listener (plugin thread)
  │                                          │
  ├── parse REST request                     ├── decode TLV frame
  │   (URL, method, headers, body)           │   (op kind, tenant, body, …)
  ├── build BridgeRequest from               ├── build BridgeRequest from
  │   request                                │   frame fields
  │                                          │
  └─────────────┐                ┌───────────┘
                ▼                ▼
                  serveDistOp(BridgeRequest)
                          │
                          ├── select service routine by req.op
                          ├── invoke service routine
                          │   (entry hook → service body → exit hook)
                          ├── translate service result into
                          │   BridgeResponse
                          └── return BridgeResponse
                                  │
              ┌───────────────────┴─────────────┐
              ▼                                 ▼
HTTP listener:                         TLV listener:
  encode BridgeResponse                  encode BridgeResponse
  as HTTP response                       as TLV reply frame
  (status, headers, body)                (msg type, correlation,
                                          status, body)
```

Concretely the inbound HTTP refactor is:

- Identify the single point in the existing HTTP pipeline where the
  request has been fully parsed (URL parsed, body parsed, @context
  resolved, tenant looked up, params validated) and the service
  routine is about to be called. That's the boundary.
- Carve a new function `serveDistOp(BridgeRequest* req, BridgeResponse* resp)`
  that takes the `BridgeRequest`, switches on `req->op`, calls the
  correct service routine with the same arguments the HTTP path
  would have used, and fills `BridgeResponse` from the service
  routine's outcome.
- HTTP path: at the boundary, build the `BridgeRequest` from the
  parsed HTTP request, call `serveDistOp`, then translate
  `BridgeResponse` back into an HTTP response (status, headers,
  body). This is the *only* HTTP-specific code on the inbound path
  after the carve.
- Bridge plugin inbound threads: decode their wire format into a
  `BridgeRequest`, call `serveDistOp`, encode `BridgeResponse` back
  into their wire format.

`serveDistOp` is also called by the upcall helpers
(`bridgeEntityUpsert`, `bridgeAttributeUpdate`) declared in §5b —
those are convenience wrappers for foreign-peer bridges (DDS / OPC-UA)
that arrive with a single attribute sample rather than a full
NGSI-LD operation.

## 8. Lifecycle

> Revised 2026-08-26 for the Bridge / Channel split of §3. Plugins are
> no longer discovered from the schemes present in the registration
> cache — they are named at startup, like every other plugin family.

```
broker boot
  ├── currentState plugin init (mongoc, …)
  ├── for each plugin named in --bridges (e.g. dds,opcua):
  │     dlopen("/opt/.../plugins/bridge/<name>.so")
  │     bridgeRegister() into the bridge registry
  ├── bridgeCache load from DB               # the Bridge OBJECTS, posted earlier
  │     for each Bridge:
  │       plugin loaded?  driver->init(bridge)   # transport comes up
  │       plugin absent?  mark unavailable       # do NOT fail the boot (§3.5a)
  ├── channelCache load from DB
  ├── config-file loader (§3.6), if a file was given:
  │     ddsmodule half  → the Bridge's config
  │     ngsild half     → Channels, with direction/retention defaults
  ├── for each Channel:
  │     driver->channelCreate(channel)    # reader/writer, subscribe, advertise
  ├── regCache load from DB
  ├── REST listener up
  └── plugin-owned threads running

broker shutdown
  └── for each loaded bridge:
        driver->close()                   # plugin tears down its threads
```

Two ordering constraints worth stating, because both are silent when
wrong. `driver->init()` must precede any `channelCreate` — a Channel is
meaningless without its transport. And the loader must run before
`channelCreate` so that file-declared and API-declared Channels take the
same path; a loader that opens DDS resources itself would be a second
implementation of the thing it is feeding.

`POST` / `PATCH` / `DELETE` of a Channel fires
`channelCreate` / `channelUpdate` / `channelDelete` on the owning driver,
so it can open, reconfigure or close the protocol resource.

Bridges fire the same three (§3.5a) — they are ordinary stored objects,
not startup-only. What differs is the cost: `bridgeUpdate` tears the
transport down and builds it again, so every reader and writer belonging
to its Channels is dropped and re-established. A duplicate `POST` never
reaches the driver; the broker answers 409 first.

Deleting a Bridge — which only happens at shutdown — cascades to its
Channels. Deleting a Channel never disturbs the transport.

## 9. Concrete bridges — roadmap

Recommended order of landing:

1. **Transport-neutral types + outbound refactor.** Define
   `BridgeRequest` / `BridgeResponse` in `corBridge`. Refactor
   every broker-to-CSR outbound call site (`ldDistOpSendMulti`,
   direct-forward sites, CSR probe) to build `BridgeRequest` and
   call `bridgeSend(scheme, req)`. HTTP becomes the inline
   implementor; no new transport yet. No wire change visible
   externally. Full functest suite green before commit.
2. **Inbound transport-neutral entry point.** Carve
   `serveDistOp(BridgeRequest)` out of the HTTP request-dispatcher;
   HTTP listener gets an adapter at the same boundary. Still no
   new transport. Full functest suite green before commit.
3. **`tlv` (first plugin, NGSI-LD peer)** — binary IPC per
   the binary-IPC notes. Both ends under our control, easiest to test
   (sw ↔ sw, fw ↔ fw, sw ↔ fw). Validates the `BridgeDriver`
   vtable, the `serveDistOp` call surface from a plugin thread, the
   `dlopen` load mechanism (`--bridges`, §4c), and the `bridgeThreadInit` upcall.
   First concrete usage of every part of the bridge family.
4. **`ws` (NGSI-LD peer)** — WebSocket. Four pieces: the HTTP upgrade
   handshake, a connection registry, message dispatch, and notification
   delivery over the open socket. The transport is well-understood and
   the work is mostly binding those to the bridge contract.
5. **`mqtt` (NGSI-LD peer)** — outbound notification path already
   exists (mosquitto integration); the bridge work unifies that
   with new inbound MQTT support for distops over a request/reply
   topic convention. Semantics need pinning (see §11) but the
   library is already a broker dep, so the .so should be thin.
6. **`dds` (translation, foreign peer)** — topics only for the
   first cut. See §9.1 below.
7. **`opcua`, `modbus`, ...** — further industrial peers, each a
   separate commit, all using the same contract.

### 9.1 DDS — topics, services and actions, client-side only

> Revised 2026-08-26. The previous scope was "topics only for the first
> cut", on the assumption that service/action semantics could wait for
> the spec. They cannot: the project driving this needs all three.

Implemented against `BridgeDriver` / `BridgeBroker.h`. Scope:

- **Topics** (pub/sub). One topic ↔ one attribute, `value` as JSON on
  the wire. Bidirectional with loop protection.
- **Services** (request/reply), **broker as client**. Invoking is a
  write to the bound attribute; the reply comes back into it.
- **Actions** (long-running goals), **broker as client**. Send a goal,
  receive feedback / status / result over time, be able to cancel.

**Client-only is a real boundary, not a first-cut limitation.** There is
no mode in which the broker answers a DDS request or executes a goal. A
context broker knows about entities, subscriptions and registrations, and
has no extension point for the business logic that would compute a reply
or run a goal — that belongs to an application, which is why every
NGSI-LD/DDS integration lands on the same boundary. It bounds the driver
contract usefully: one initiator, and for actions several arrivals over
time plus a cancel.

Weight, for planning — a working implementation of this scope runs to
roughly 5 kLOC, split about 42% topics and shared core, 25% services,
33% actions. So services and actions are the larger part, and they are
also the part whose NGSI-LD representation is provisional (§10).

Two operational consequences of services and actions, both absent for
topics:

- **No type discovery.** There is no peer to learn the IDL from before
  the first send, so pre-built `.bin` type files must be on disk. The
  `typesDirectory` becomes a Bridge field.
- **A synchronous timeout** for service invocation — `syncTimeoutMs`,
  a Bridge default overridable per Channel.

Port the DDS *mechanics* — type loading, `.bin` handling, goal
correlation, cancel, feedback/status/result arrival. That is transport
work which has to exist in any design and is where the value of those
5 243 lines actually sits. What gets rebuilt is where it lands on the
NGSI-LD side.

⚠ **The anti-pattern to avoid**, because it is the obvious shortcut and
it is wrong here: fabricating per-request state on the inbound path —
filling in the request struct, setting the URL wildcards, calling a
service routine directly and then invoking the request-completed hook by
hand, all from a transport callback thread. Per-request state belongs in
the per-connection struct, and the one-thread-per-connection invariant
holds. The inbound path goes through `serveDistOp()` — step 2 of §9 —
which is why that step is not optional for this bridge.

## 10. Service Execution — a provisional convention, quarantined

> Revised 2026-08-26. Was "deferred". Deferral is no longer available:
> the DDS bridge needs services and actions now, and they need *some*
> NGSI-LD representation.

The next NGSI-LD release will introduce a first-class **Service
Execution** concept, and it will settle how an invocation, a reply,
feedback, a result and a status appear in the model. It supersedes
TS 104 175 Annex G, which today describes actuation as two *suggested*
workflows (§G.5 subscription-based, §G.6 registration-based) built out
of existing primitives, informatively — not as anything the API itself
offers. Until then
something has to be chosen, because a client that invokes a DDS service
has to read the answer somewhere.

The question worth optimising is therefore not how to avoid choosing —
that option is gone — but **how cheap the choice is to replace.**

So:

1. **Adopt the convention already in field use, explicitly provisional.**
   `ddsServiceReply` / `ddsActionFeedback` sub-Property envelopes,
   per-goal instances keyed by `datasetId`. The reason is
   **interoperability**: deployments and tooling exist that already read
   that shape, and matching them is the point. Inventing a better one now
   would buy nothing and would be harder to abandon later.
2. **Model the kind from day one.** A Channel declares
   `kind: topic | service | action` (§3.3) whether or not the semantics
   are final. The model is then complete and the driver vtable knows what
   it will eventually need.
3. **Quarantine the convention behind the driver.** Nothing outside the
   bridge plugin should encode what a feedback envelope looks like. When
   Service Execution lands, what changes is a translation layer, not the
   model — and not eleven service routines.
4. **Write down that it is provisional**, in the plugin's own docs and in
   whatever the API returns, so nobody builds on it believing otherwise.

**What is and is not exposed**, since "an external consumer might parse it"
is too broad to act on. It splits three ways and only one carries risk:

| Mode | What a consumer reads | Exposed to the change? |
|---|---|---|
| **Topic** | `attribute.value` — the sample itself | **No.** That is the application's own payload travelling over the wire, not a convention of ours, and Service Execution does not touch it. |
| **Service** | the reply, merged back into the attribute | Only if it reads a named sub-Property rather than the value. |
| **Action** | per-goal instances plus feedback / result / status sub-Properties | **Yes — all of it is convention.** |

**On the action convention's use of `datasetId`** — a fairer reading than
"it overloads a mechanism", which an earlier draft of this section claimed.
`datasetId` distinguishes *multiple instances of one attribute*, and N
concurrent goals genuinely are N instances of one attribute. That is a case
the definition covers rather than a mechanism bent out of shape, the
`urn:goal:` prefix namespaces it so ordinary datasetIds keep their meaning
alongside, and it buys storage, query and temporal handling for nothing. It
was chosen deliberately, and as provisional from the start.

⚠ **What it does not buy is notification scoping, and that is the part to
watch.** A subscription on an action attribute fires on every change, but
the notification body carries the attribute's default instance rather than
the changed goal's feedback, result or status — there is no datasetId-scoped
projection on the notification path. So the one thing a goal most needs,
*tell me when this goal progresses*, is exactly what the borrowed mechanism
cannot do; a consumer falls back to `GET …?datasetId=urn:goal:<uuid>` or the
temporal API. The lifecycle is unusual too: the attribute disappears when
the last goal finishes and returns with the next, which no ordinary multi-
instance attribute does.

Being provisional by intent, the migration when Service Execution lands is
closer to a rename than a re-architecture — which is why quarantining behind
the driver is enough, and no stronger claim is needed to justify it.

Which makes the question to ask a deployment much more specific than whether
anything parses the envelopes: *does it track a goal while the goal runs —
reading feedback or status — or does it only read the final value once the
goal is done?* If the latter, the convention is invisible to it and can be
replaced at will.

When Service Execution does land, the `BridgeDriver` contract grows a
parallel vtable section for invocation, mirroring the spec API. OPC-UA's
`Call` services map onto the same abstraction.

## 11. Open questions

- **URI scheme registry**: do we publish a `coraine URI
  scheme conventions` doc, or borrow from existing IANA / industry
  conventions where they exist (`opc.tcp://`, `modbus://`, etc.)?
- ~~**CSR `endpoint` semantics for bridges**~~ — **answered 2026-08-26
  by §3.1.** A bridge endpoint is no longer a registration's
  `endpoint`, so the "URI a broker can forward to" reading never
  applies to it. Nothing to exempt and no sentinel needed.
- **Tenant scope**: a Channel is tenant-scoped like any other
  broker-held object. Does each tenant get
  its own DDS participant, or is there one participant per broker
  serving all tenants with topic-name prefixing? Probably per-tenant
  for isolation, but the cost of N participants needs measuring.
- **Outbound loop prevention across multiple bridges**: if a bridge
  receives a sample, upserts the entity, and the upsert fans out to
  *other* bridges via distops, do we want it to? Preventing only the
  SAME-bridge loop is the minimum and is what existing integrations do;
  per-scheme allow-/block-listing on the Channel might be needed. Note
  that `retention: relay` (§3.3) changes the shape of this — a relay
  Channel stores nothing, so there is no local write to fan out from in
  the first place.
- **MQTT pub/sub vs request/reply for distops**: NGSI-LD distop
  semantics are request/reply (peer broker returns a response body
  + status). MQTT is fundamentally pub/sub. Two options:
  (a) **One-shot topic pair per request**: requester publishes to
  `<prefix>/req/<correlation-id>`, subscribes to
  `<prefix>/resp/<correlation-id>`, unsubscribes when response
  arrives. Cheap on the wire, expensive on the broker subscription
  table for high-volume use.
  (b) **Per-broker request/response topic pair**: every broker has
  one inbound topic and one outbound topic, correlation handled by a
  correlation id inside the payload. Cheaper subscription table,
  requires a side-table of pending requests on the requester.
  Probably (b). Pin before any MQTT bridge code lands.
- **Distop dispatcher refactor**: how invasive is the change to
  `ldDistOpSendMulti` to add the `bridgeSend(scheme, req)` dispatch?
  The transport-neutral refactor (step 1 in §9) is large enough on
  its own — landing it as a self-contained commit before any new
  transport plugin keeps the bridge PR small.
- **Channel conflict beyond registrations** (new 2026-08-26): two
  mirroring Channels claiming the same `(entity, attribute)` from
  different transports. Refuse, or allow and define ordering? Refusing
  is the conservative default and can be relaxed later; allowing cannot
  be tightened later without breaking deployments.
- **Does anything external parse the v0 service/action envelopes?**
  (new 2026-08-26) — §10 rests on the answer. If a partner's code reads
  `ddsActionFeedback` directly, it is an interface and the migration to
  Service Execution stops being free.
- **Notification transport unification**: the broker's current MQTT
  support is for outbound subscription notifications (separate from
  the distop / CSR path). Bringing notifications under the bridge
  family would unify "broker sends a thing to a remote endpoint" but
  would also expand the bridge scope beyond CSR-bound traffic. For
  the first cut the bridge family is CSR-bound only; notifications
  stay in the existing notification path. Re-evaluate after WS / MQTT
  bridges are working — both protocols naturally carry both kinds of
  traffic on the same connection.

---

**Naming**: this document was written before the 2026-08-19 rename and moved
here after it. Paths and identifiers now use the current names throughout —
`coraine`, `corLibs`, `corNgsild`, `corRest`, `corDB`.

**Related docs**:
- Speaking to devices directly — the south bridge, and why it is this same
  family rather than a second one: [`device-protocols.md`](device-protocols.md)
- What FIWARE's IoT Agents actually do, and how they map onto Bridge /
  Channel / registration: [`iot-agents.md`](iot-agents.md)
- The plugin architecture the bridge family sits beside:
  [`plugin-architecture.md`](plugin-architecture.md)

The wire format for the binary IPC plugin, and the original candidate analysis
that preceded it, are in a separate working document not published here.
