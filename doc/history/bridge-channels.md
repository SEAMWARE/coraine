# Bridge channels - history

The story behind [bridge-channels.md](../bridge-channels.md): the drafts, the
direction changes and what was tried and dropped. The current design and
reference is [bridge-channels-details.md](../bridge-channels-details.md).

Dated entries, newest first. The notes started as a working design document;
this is what it recorded about itself along the way. A few undated passages
close the page.

## 2026-09-26 - topics are DDS first (§ 9.1)

A value that did not fit its topic's type was stored and answered 204, and
only the log said it never reached DDS - Orion-LD's behaviour too (its
functest `dds_publish_patch_attribute-with-invalid-value` pins it). Now the
sample is published before the write, by the same step that sends service
requests and goals (`bridgeRequestsBeforeWrite`), and a refused one writes
nothing: 400 for a value that does not fit, 503 for a topic not yet announced.
Nothing is sent after a write any more; `bridgeAttrOut` / `bridgeAttrsOut` are
gone, and with them the batch paths' "publish only on DB_OK" - a batch now
sends for Entities known to exist (or, for create, known not to), before the
bulk write, as it already did for goals.

KZ, 2026-09-26, on the § 9.1 entry *"A topic: DDS first too"*: until then
topics were NGSI-LD first - stored, then published, and a refused publish was
a warning in the log behind a 204 - as Orion-LD does.

## 2026-09-25 - an action's transport DECIDES before the broker stores (§ 9.1)

"DDS first" used to mean only that a goal was *sent* before the write; the
write then happened whatever the server said, so a rejected goal left its
request stored behind a 422 (found by the goal resource's test). Now the write
waits for the goal's first answer, and a refused, lost or unanswered goal
writes nothing. It also removes the window both 2026-09-24 races lived in - an
answer landing between send and write - and with it the holding of goal
events. Services and topics are unchanged. Bridge ABI 5 (same date) adds
`BridgeGoalPart` and `BridgeBroker.goalEventPartIn`: a plugin says which part
of a goal an event's payload is (status, feedback, result), so the broker can
show `goalFeedback` / `goalResult` whatever the transport calls them.

Two § 9.1 entries were first written as *"agreed 2026-09-25, not yet built"*:
the action that waits for the transport's decision, and a goal's
notifications as a normal `Notification` from a subscription. The per-goal
cache-only subscription was then marked *"built, phase 1"*, and the defaults
(Channel's, then Bridge's) given to a goal as its endpoint *"built,
2026-09-25"*.

## 2026-09-23 - `ddsSync`, and bridge ABI 3

A service request can now be WAITED FOR (§ 9.1), which the seam could not
express: an asynchronous reply is identified by its endpoint alone, and a
waiting request must get its own reply and no other — not a late answer to an
earlier invocation, not one belonging to a request that already gave up.
ABI 3 appends `BridgeDriver.serviceInvokeTracked(endpoint, json, token)` and
`BridgeBroker.replyIn(..., token, ...)`. The BROKER chooses the token, because
it has to be waiting before the reply can arrive, and a reply can arrive
before the invocation returns. A reply to a request that timed out is dropped,
so that a request that answered 504 never gains a reply afterwards.

## 2026-09-18 - the plugin contract and conditional compilation (§ 2b)

On the edge build (a conditionally compiled coraine), the notes first got the
plugin contract backwards and KZ corrected it: they said that a
layout-changing feature would make an edge broker incompatible with its bridge
`.so`, and proposed a build-configuration check. Wrong problem. Checked
afterwards: no `COR_FEATURE` and no conditional of any kind appears inside the
driver structs, so the vtable is the same shape in every build and a feature
that is off is a NULL function pointer. The rule kept is *never put a `#if`
inside a driver struct*.

## 2026-09-16 - timing and status, which were never written down

**coraine implements this design now; it is not waiting for ETSI.** The
concept goes to the TC DATA face-to-face in Athens, 20–22 October 2026, and
anything normative that follows will realistically be 2027. DDS is needed long
before that, so the Bridge and Channel objects are **coraine's own**, to be
adapted to whatever TC DATA settles on rather than held back until it does.
That is the cheaper direction to be wrong in: an implementation can be
aligned, and a specification nobody has implemented cannot be validated. What
this means for readers of these notes: nothing in them is standard NGSI-LD,
the names may change, and the endpoint-scheme convention is the part most
likely to survive.

## 2026-08-29 - the object called a *Binding* is now a **Channel**

"Binding" is taken in TC DATA at document-title level (TS 104 176 *"NGSI-LD
API Bindings"*, TS 104 243 *"MQTT Notification Binding"*), where it means how
API operations are conveyed over a protocol; ours means how a value is carried
to and from a foreign endpoint. The reasoning, and why *Map* is worse, is in
§ 3. Also settled: the **three clocks** that the lifecycle question kept
conflating (§ 3), a **`status`** on both objects so the degraded states are
observable rather than silent (§ 3.3a), **no runtime plugin loading**
(§ 3.5c), and the apparent contradiction between § 4c's "fail loudly" and
§ 3.5b's "boot anyway", which are different cases (§ 4c). Source layout
answered too: **one library per bridge**, sibling to the other Cor-Libs, with
the contract headers in a small `corBridge` of their own (§ 4a). The § 4a
layout was confirmed on this date.

## 2026-08-27 - the model checked beyond DDS; the Bridge is not read-only

The model of § 3 was derived from DDS, and DDS is unusually well-behaved.
Tested against the other protocols a south bridge must carry, it holds for
**addressed endpoints** (OPC-UA nodes, LWM2M resources, Modbus registers) and
**breaks for bundle sources** (UltraLight, JSON, CSV, LoRaWAN, Sigfox, Kafka),
where one arriving message sets many attributes on an entity it names itself.
Consequences: a Channel gains a **codec**, device **provisioning turns out to
be a shape of Channel** rather than a layer above it, and a Bridge may be a
listener as well as a client. See § 3.8.

Also corrected: the Bridge resource is **not** read-only. The bridge *kind* is
startup-fixed; the *instance* is a stored object with full CRUD, a 409 on a
duplicate id, and an orphan case to answer — § 3.5a. § 3.5b settles how a
Bridge names its plugin (short name, never a path in the payload), why the
broker does not auto-create one from a plugin it finds, that both objects
carry an `id` and a `type` like every other stored body, and that a **Channel
names its Bridge explicitly** — the endpoint scheme stops being a selector as
soon as two Bridges of a kind exist.

The earlier draft's mistake, as the notes recorded it in three places:

- An earlier draft made the Bridge read-only because the *kind* is
  startup-fixed (§ 3.5a) — that was the first row's property (the
  Capability's) applied to the second (the Bridge's).
- An earlier draft made the Bridge resource read-only, on the grounds that
  bridges are not created at runtime. That conflated two things: the *kind*
  and the *instance*.
- An earlier draft had Bridge read-only over the API, which was the
  Capability row's property mistakenly applied to the next one.

## 2026-08-26 - the CSR-based mapping convention is **dropped**

Bridge endpoints are no longer Context Source Registrations. The mapping
becomes two objects of its own — a **Bridge** (the transport instance) and a
**Channel** (one foreign endpoint tied to one entity attribute) — because a
registration makes a claim about the world that a bridge does not make. The
endpoint **URI scheme convention is still shared** across registrations,
subscriptions and bridges; only the object is not. § 9.1 also widens: DDS
services and actions are in scope after all, client-side only, and § 10
changes from "defer them" to "adopt a provisional convention and quarantine
it". See § 3, § 9.1, § 10.

What the revision changed, as noted in each section:

- **§ 3**: the 2026-08-26 draft called the new object a **Binding** (renamed
  Channel on 2026-08-29).
- **§ 4, `BridgeDriver`**: the revision reshaped the struct rather than
  appending. Nothing was implemented yet, so there was no ABI to break — the
  append-only rule started applying at the first release. The Channel
  lifecycle hooks `channelCreate` / `channelUpdate` / `channelDelete` were
  `csrCreate` / `csrUpdate` / `csrDelete` before it.
- **§ 4c**: auto-discovery of plugins from the persisted regCache was dropped
  along with the CSR-based mapping of § 3.
- **§ 8**: revised for the Bridge / Channel split. Plugins are no longer
  discovered from the schemes present in the registration cache — they are
  named at startup, like every other plugin family.
- **§ 9.1**: the previous scope was "topics only for the first cut", on the
  assumption that service/action semantics could wait for the spec. They
  cannot: the project driving this needs all three.
- **§ 10**: was "deferred". Deferral is no longer available: the DDS bridge
  needs services and actions now, and they need *some* NGSI-LD
  representation.
- **§ 11**: the open question *"CSR `endpoint` semantics for bridges"* was
  answered by § 3.1. A bridge endpoint is no longer a registration's
  `endpoint`, so the "URI a broker can forward to" reading never applies to
  it. Nothing to exempt and no sentinel needed. (§ 3.1a: "the § 11 question
  about `endpoint` having to be a reachable URI dissolves — `dds://rt/pose` is
  not a registration's endpoint any more.") Two questions were added: Channel
  conflict beyond registrations, and whether anything external parses the v0
  service/action envelopes.

## 2026-08-19 - the rename

The notes were written before the 2026-08-19 rename and moved into this
repository after it. Paths and identifiers were then changed to the current
names throughout — `coraine`, `corLibs`, `corNgsild`, `corRest`, `corDB`.

## 2026-05-25 - direction change: HTTP **stays inline**

HTTP stays in the broker, not in a plugin. The earlier "HTTP refactor first as
a no-op move into `http.so`" recipe is dropped. The bridge family is now
specifically for **non-HTTP** transports of broker-to-CSR traffic. What the
bridge work actually adds inside the broker is a transport-neutral
request/response shape (`BridgeRequest` / `BridgeResponse`) and a single
inbound entry point (`serveDistOp(BridgeRequest)`) that both the HTTP listener
and any bridge plugin's inbound thread converge on. HTTP is then "the one
inline implementor of the bridge interface" — it's just not packaged as a
`.so`. See § 1 and § 9 for the updated framing.

This revision's mapping convention said *"every bridge endpoint is a Context
Source Registration with a protocol-specific endpoint URI"*. It isn't, and
§ 3.1 is why; the 2026-08-26 revision superseded it.

## 2026-05-22 - first draft

The draft history ran to six revisions between 2026-05-22 and 2026-09-16,
including two direction changes, kept at the end of the notes because it
records what was tried and rejected, which is what stops a settled question
from being re-opened. They were written newest-first as they happened (the
2026-05-25 entry sat out of order between 2026-09-16 and 2026-08-26; it is in
date order here).

## Undated

**How the notes described themselves.** "Working design notes, not a user
manual: they carry the reasoning, the wrong turns and the open questions,
because those are what stop a settled decision from being re-argued. **Not
implemented.**" (Parts are implemented now; the details doc says which.)

**The DDS port, as planned (§ 9.1).** Weight, for planning — a working
implementation of this scope runs to roughly 5 kLOC, split about 42% topics
and shared core, 25% services, 33% actions. So services and actions are the
larger part. Port the DDS *mechanics* — type loading, `.bin` handling, goal
correlation, cancel, feedback/status/result arrival. That is transport work
which has to exist in any design and is where the value of those 5 243 lines
actually sits. What gets rebuilt is where it lands on the NGSI-LD side.

**Traces compiled out of `dds.so` (§ 3.3b).** A `dds.so` with its traces
compiled out made every DDS test time out while the broker worked - the tests
were waiting on trace lines. That is why the counters exist for tests to wait
on ([Testing](../testing.md#functional-tests-never-read-the-log)).

**The config-file loader (§ 3.6).** An earlier framing had the loader as
scaffolding to delete when the project that motivated it ends. It stays: the
format is published and in use.

**Source layout (§ 4a).** With one repo per bridge, the old "thin ones stay as
sub-libs in the umbrella" rule has nothing left to attach to.

**`datasetId` for goals (§ 10).** An earlier draft of § 10 claimed the action
convention's use of `datasetId` "overloads a mechanism". The fairer reading,
now in § 10, is that N concurrent goals genuinely are N instances of one
attribute.
