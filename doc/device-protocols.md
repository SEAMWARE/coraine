# Speaking to devices directly

> Planned, not built. This describes where the broker is going and why the shape
> is what it is; nothing here exists yet. Tracked from the
> [roadmap](roadmap.md).

A FIWARE deployment normally has two tiers. Devices speak their own protocols —
MQTT with an UltraLight payload, LWM2M over CoAP, OPC-UA, LoRaWAN — to an **IoT
Agent**, which translates them into NGSI-LD and forwards to a **context broker**.
One agent per protocol family, then the broker.

That split exists because brokers have not spoken those protocols. It is a
property of the implementations, not of NGSI-LD, and it is worth removing as a
*requirement* while keeping it as an *option*.

## The capability, not the product

The thing to build is a **south bridge**: the broker itself able to talk to
devices, as a family of plugins. Once that exists, the deployment tiers become a
build and configuration choice rather than an architecture:

| Shape | What runs | When it fits |
|---|---|---|
| **Agent + broker** | A small broker build with the south bridge at the edge; a full broker centrally, no device protocols compiled in | The classic topology. Many sites, a lot of devices, agents near them. |
| **One broker, both jobs** | A single broker with the south bridge and the full NGSI-LD API | A small installation. FIWARE@Home on a Raspberry Pi: one process, the devices in the flat, no tier to operate. |
| **Broker only** | No south bridge compiled in | A cloud broker fed by agents, or by other brokers. |

The middle row is the point. A single apartment does not need a device-protocol
tier, a context tier and the operational surface of both — it needs one binary
that reads the sensors and answers NGSI-LD. And a deployment that starts there
and grows should not have to change products to split the tiers later; it should
change how it is built and where the pieces run.

**cor-agent** is the name for the first shape's edge build — the broker compiled
down by the per-feature `#ifdef`s, shipped as its own image. It is a build
configuration of this broker, not a separate codebase, which is the whole reason
the reuse is worth having. See *Embedded deployment* in the
[roadmap](roadmap.md): running on constrained hardware is already a build
question rather than a redesign.

## It is the bridge family, not a second one

A device protocol and a foreign peer are the same problem. A DDS topic carrying
a robot's pose and an MQTT topic carrying a thermostat's temperature both mean:
*something out there that does not speak NGSI-LD holds a value that belongs in an
attribute; translate at the boundary, in one or both directions.*

So this reuses the bridge model rather than inventing another:

- A **Bridge** is a transport instance — the MQTT connection, the CoAP server,
  the DDS participant. Few, fixed at startup.
- A **Binding** ties one foreign endpoint to one entity attribute, with a
  direction and a retention policy.

"North" and "south" describe where a deployment puts things, not two mechanisms.
The broker has one way of attaching a foreign endpoint to an attribute, and a
device is a foreign endpoint.

### Flow or delegation — and the spec already names both

There are two ways a device's value can reach the broker, and they are
not a matter of taste:

| The value… | Modelled as | The spec's workflow |
|---|---|---|
| flows through the broker — pushed by the device, or pushed to it | a **Binding** | subscription/notification (§G.4.2, §G.5) |
| lives at the device or its adapter, fetched when read | a **registration** whose endpoint names a bridge scheme | forwarding (§G.4.3, §G.6) |

TS 104 175 Annex G, *Suggested actuation workflows*, describes both for
what it calls a **Context Adapter** — see
[FIWARE IoT Agents](iot-agents.md) for the detail, including the
provisioning model, the four attribute kinds and the command lifecycle: one where the adapter "acts as a
Context Source as well as a Context Consumer" and holds no store, and one
where it registers "I am responsible for command property X" and "acts as
a Context Storage as well as a Context Producer".

**Lazy attributes are why the second exists.** A device's battery level
should be read when someone asks, because polling it drains the thing
being measured. The broker holds nothing and fetches on demand — which is
what a registration means. Emulating that over the push model (notify the
adapter "go and read this", wait for it to write the value back) works and
is strictly worse: the client's `GET` waits on a round trip through a
notification and a subsequent write, where forwarding does it in one hop.

One protocol often wants both. OPC-UA monitored items push; OPC-UA reads
pull. LWM2M observes and reads. The choice belongs per attribute, not per
protocol.

⚠ Annex G is informative — a suggested workflow, not a mechanism the API
provides. That is the gap **Service Execution** closes, and when it lands
this section changes with it. The distinction should survive the change:
flow versus delegation is a fact about where a value lives.

One piece genuinely is new: **device provisioning**. FIWARE's agents expose an
API for registering a device and mapping its readings to entity attributes, in
bulk and from templates, with service groups and keys. In this model that is a
layer over Bindings — provisioning a device creates them — but whether it is a
thin convenience or its own concept is not yet decided, and the same question
about not overloading an existing mechanism applies.

## Two axes, not one list

The protocol list is long. The plugin count should not be, because FIWARE's own
agent library already draws the line in the right place: it configures device
communications *"regardless of the payload, syntax or transport protocol
used"*.[^lib]

Splitting the same way turns a product into a sum:

| Transports | Payload codecs |
|---|---|
| HTTP, MQTT, AMQP, CoAP, Kafka, DDS | JSON, UltraLight 2.0, LWM2M objects, ISOXML/ADAPT, CSV |

An UltraLight payload over MQTT and the same payload over HTTP are one codec and
two transports, not two agents. Vendor protocols that do not decompose — LoRaWAN
and Sigfox carry their own network semantics, not just a payload — stay whole.

## Parity is the floor, not the goal

An IoT Agent is a southbound protocol adapter, not an NGSI endpoint. It
provisions devices, translates their measurements into `updateContext` calls
against a broker, and answers broker-initiated requests as a registered context
provider for lazy attributes and commands. It does not serve subscriptions or
registrations of its own, does not host context or answer queries, and has no
temporal API.[^surface]

That is not a criticism — it is the correct shape for a component whose job is
translation, and it is why an agent must be paired with a broker to be useful at
all. Note that it *uses* subscriptions and registrations, as Annex G describes;
it does not *serve* them.

But it means a broker doing the same job is not the same function in fewer
processes. It is strictly more function, and none of it has to be built, because
the broker already is one:

- **Subscriptions fire at the edge.** A rule that reacts to a sensor can run
  where the sensor is — no round trip, and it keeps working while the uplink is
  down.
- **Context is hosted locally.** Current device state is queryable on the spot
  with the whole NGSI-LD query language — `q`, geo-queries, `attrs`, projections
  — rather than only being forwarded onward.
- **Registrations work in both directions.** The edge broker can be a context
  source for a central one, or federate with peers, using the same mechanism
  everything else uses.
- **Temporal history at the edge**, through the same TRoE plugins.
- **Snapshots, tenants, JSON-LD `@context` handling** — everything the broker
  already does, available at the point the data arrives.

The honest trade is size and operational surface: an agent is a smaller thing to
run, and a deployment that genuinely only needs protocol translation is carrying
more machinery than it uses. That is what the conditional compilation is for —
the edge build should contain what that deployment turned on and nothing else.

## What parity requires

So that using this instead is never a loss of function:

| Existing agent | Carries |
|---|---|
| IoT Agent for JSON | JSON payloads over HTTP/MQTT |
| IoT Agent for UltraLight | UltraLight 2.0 over HTTP/MQTT |
| IoT Agent for LWM2M | Lightweight M2M over CoAP |
| IoT Agent for OPC-UA | OPC Unified Architecture |
| IoT Agent for LoRaWAN | LoRaWAN networks |
| IoT Agent for Sigfox | Sigfox networks |
| IoT Agent for ISOXML | ISOXML/ADAPT, agricultural machinery, over HTTP |

What each of those actually does — provisioning, active and lazy attributes,
commands and their status lifecycle — is written up in
[FIWARE IoT Agents](iot-agents.md).

Beyond parity: **DDS** (already in progress as a bridge), **MQTT** as a transport
in its own right rather than only under a payload agent, **CSV**, and probably
**Kafka**.

Two things from the agent world are not protocols and still have to be answered:
**device provisioning** (above) and **commands** — an agent registers itself as
the context provider for a command attribute so that writing it reaches the
device. When the broker owns the south bridge there is no second component to
register with, and a command becomes an outbound Binding. Which is the same
mechanism as everything else here, but the API a device integrator sees for it
is not yet designed.

## Order

After the binary IPC protocol and corDB, and after the per-feature conditional
compilation that makes a small build possible at all. The bridge family lands
first with DDS; the device protocols are more plugins on the same contract, which
is the point of doing DDS first.

[^surface]: [`iotagent-node-lib` API documentation](https://iotagent-node-lib.readthedocs.io/en/latest/api.html)
    — the agent is "a common abstraction layer between the devices and the NGSI
    entities stored in Context Broker", forwarding measurements and acting as a
    registered context provider for lazy attributes and commands.

[^lib]: [`iotagent-node-lib`](https://github.com/telefonicaid/iotagent-node-lib).
    The agent list is from the FIWARE catalogue: [OPC-UA](https://github.com/FIWARE-GEs/iotagent-opcua),
    [ISOXML](https://github.com/FIWARE/iotagent-isoxml) and the JSON, UltraLight,
    LWM2M, LoRaWAN and Sigfox agents alongside them.
