# FIWARE IoT Agents — what they are and how they work

Reference notes, gathered 2026-08-26. Background for
[Speaking to devices directly](device-protocols.md) — the planned work that lets
coraine reach devices without a separate agent tier.

This describes components that are not ours, written down so the design work does
not have to re-derive it. FIWARE's IoT Agents are the established way to get
device data into a Context Broker, and understanding them properly is what makes
it possible to say precisely what changes when the broker does the job itself.
Provenance for every non-obvious claim is at the end; where something was not
verified, it says so.

---

## 1. What an IoT Agent is

A translator between a device protocol and NGSI. Devices speak MQTT/UltraLight,
CoAP/LWM2M, OPC-UA, LoRaWAN, Sigfox…; the agent turns that into NGSI entity
updates against a Context Broker, and turns broker-side writes back into device
commands.

Nearly all of them are built on **`iotagent-node-lib`**, which is a library, not
a product. It supplies the provisioning API, the device/group model, the
northbound NGSI interactions and the command machinery; each agent adds a
southbound binding and a payload codec.

### 1.1 What it is *not* — the point that matters most

An IoT Agent is **a southbound protocol adapter, not an NGSI endpoint**. It:

- provisions devices and config groups over its own REST API,
- forwards measurements to a broker as `updateContext`,
- answers broker-initiated requests as a **registered Context Provider**, for
  lazy attributes and commands.

It does **not** serve subscriptions or registrations of its own, does not host
context or answer arbitrary queries, and has no temporal API.

It *uses* subscriptions and registrations; it does not *serve* them. An agent is
therefore useless without a broker behind it — which is the whole reason a broker
that owns the south bridge is strictly more capable than an agent, rather than
the same function in fewer processes.

---

## 2. The spec's own account: TS 104 175, Annex G

The two integration conventions are not folklore — they are written down.
**Annex G, "Suggested actuation workflows"** (informative) calls the agent a
**Context Adapter** and gives two communication models in §G.4.1:

| Model | Spec clauses | The adapter acts as | State | Delivery |
|---|---|---|---|---|
| **Subscription / notification** | §G.4.2, impl §G.5 | "a Context Source as well as a Context Consumer" | none of its own | subscriptions carry command requests to it; it writes results back |
| **Forwarding** | §G.4.3, impl §G.6 | "a Context Storage as well as a Context Producer" | **its own**, possibly on the device itself | it registers "I am responsible for command property X"; the broker forwards |

The forwarding model is what makes **lazy attributes** possible at all: an
attribute read only when someone asks, because reading it costs something.

⚠ Annex G is **informative**, and describes workflows assembled from existing
primitives rather than a mechanism the API provides. That is the gap **NGSI-LD
Service Execution** is meant to close — and when it lands, most of this annex is
superseded.

---

## 3. The provisioning model

Two resources, both under `/iot`, both outside NGSI:

| Resource | Endpoints |
|---|---|
| **Config groups** (historically *service groups*) | `GET`/`POST`/`PUT`/`DELETE /iot/groups` |
| **Devices** | `GET`/`POST /iot/devices`, `GET`/`PUT`/`DELETE /iot/devices/:deviceId`, `POST /iot/op/delete` for batch removal |

- A config group is identified by an **`apikey` + `resource` pair**, which must
  be unique. The `apikey` authenticates requests arriving from devices and says
  which group they belong to.
- Multi-tenancy uses the **`FIWARE-Service`** and **`FIWARE-ServicePath`**
  headers.

A group supplies defaults; a device may override them. This is the piece with no
counterpart in a broker: it is a device registry, and it is why "provisioning" is
the one genuinely new concept the south bridge has to answer.

---

## 4. Attribute kinds

A device declares four, and the distinction drives everything else:

| Kind | Provisioning key | Meaning |
|---|---|---|
| **Active** | `attributes` | Measures **pushed** from the device. The agent sends them to the broker as `updateContext`. |
| **Lazy** | `lazy` | Passive measures **pulled** on demand — the agent is the registered Context Provider, and the broker forwards the read. |
| **Static** | `static_attributes` | Fixed values held in the broker, never updated by the device. |
| **Commands** | `commands` | Actions invoked *on* the device. The broker writes the attribute; the agent delivers it southbound. |

**Active ↔ lazy is the flow-versus-delegation split**, in the agents' own
vocabulary. Active attributes flow through and the broker holds them. Lazy
attributes live at the device and are fetched — the battery-level case, where
polling is what drains the thing being measured.

---

## 5. Commands

Writing a command attribute triggers southbound delivery. The agent maintains
two auxiliary attributes per command, named after it:

| Suffix | NGSI type | Carries |
|---|---|---|
| `<cmd>_status` | `commandStatus` | where the command has got to |
| `<cmd>_info` | `commandResult` | the result — retrieved information, or the outcome of an actuation |

Status values, verbatim from the library's own documentation:

| Value | Meaning |
|---|---|
| `UNKNOWN` | "This is the initial value." |
| `PENDING` | "In a PUSH command means that command has been sent to device but not device has still not respond. In a PULL command means that command has been stored and device still has no ask for it." |
| `DELIVERED` | "The command has been delivered to phisical device." |
| `OK` | "The command has been delivered and device has respond." |
| `ERROR` | "There is a kind of error." |
| `EXPIRED` | "This meens that pull command has been expired without be delivered to device according with `pollingExpiration` time defined by config." |

**PUSH vs PULL** is about whether the agent can reach the device or must wait for
the device to call in. A sleeping, battery-powered or NAT'd device polls; the
command is stored until it asks, and expires if it never does. That asymmetry has
no equivalent in a broker today and is worth remembering when designing outbound
Channels.

With `cmdMode: notification`, the entity is created at provisioning time with the
command attributes set to `null`, meaning "not yet triggered".

---

## 6. The agent landscape

| Agent | Carries |
|---|---|
| IoT Agent for **JSON** | JSON payloads over HTTP/MQTT |
| IoT Agent for **UltraLight** | UltraLight 2.0 over HTTP/MQTT |
| IoT Agent for **LWM2M** | Lightweight M2M over CoAP |
| IoT Agent for **OPC-UA** | OPC Unified Architecture |
| IoT Agent for **LoRaWAN** | LoRaWAN networks |
| IoT Agent for **Sigfox** | Sigfox networks |
| IoT Agent for **ISOXML** | ISOXML/ADAPT, agricultural machinery, over HTTP |

Seven, plus `iotagent-node-lib` itself. That is the parity bar.

---

## 7. Transport and payload are separate axes

The library configures device communications *"regardless of the payload, syntax
or transport protocol used"* — and that line is the most useful thing in its
documentation. The seven agents above are not seven independent things; they are
mostly combinations:

| Transports | Payload codecs |
|---|---|
| HTTP, MQTT, AMQP, CoAP | JSON, UltraLight 2.0, LWM2M objects, ISOXML/ADAPT |

UltraLight over MQTT and UltraLight over HTTP are one codec and two transports.
**LoRaWAN and Sigfox do not decompose** — they carry network semantics, join
procedures and operator back-ends, not merely a payload — so they stay whole.

For the south bridge this is a sum rather than a product, which is the difference
between a long protocol list and a long plugin list.

---

## 8. How it maps onto coraine's model

The south bridge models a foreign endpoint tied to an entity attribute as a
**Channel** on a **Bridge** (a transport instance), and a value that lives at the
peer and is fetched on read as an ordinary **registration** whose endpoint names
a bridge scheme. See [Speaking to devices directly](device-protocols.md).

| Agent concept | Our model |
|---|---|
| Active attribute | **Channel**, `direction: in`, `retention: mirror` |
| Command | **Channel**, `direction: out` |
| Lazy attribute | **registration** with a bridge-scheme endpoint — the broker holds nothing and fetches on read |
| Static attribute | an ordinary attribute; nothing to model |
| Config group + device | **device provisioning** — a layer over Channels, not yet designed |
| `<cmd>_status` / `<cmd>_info` | no equivalent yet; closest is the provisional convention planned for bridge services and actions, and Service Execution should settle both together |

The last two rows are the open work. Everything above them already has a home.

---

## 9. Not verified

- **Transport bindings of the library itself.** HTTP and MQTT are documented on
  the agents; AMQP appears in the ecosystem, but the library's own binding list
  was not confirmed from primary documentation.
- **NGSI-LD support.** The library is described as working with both NGSI-v2 and
  NGSI-LD, and how that is selected was not confirmed. Relevant, because a
  NGSI-v2-only agent is a migration question rather than a parity question.
- **Whether every agent in §6 is maintained**, and at what NGSI version. The list
  is from the FIWARE catalogue; adoption and health were not checked.
- **Provisioning payload details** beyond the attribute kinds — `entity_name`,
  `entity_type`, `timezone`, `expressionLanguage`, transformation expressions —
  read about but not verified field by field.
- **The southbound protocol details of any individual agent.** This document is
  about the pattern, not any one implementation.

## Sources

- **Annex G** — ETSI TS 104 175, `md/annex-g.md` in the specification's own git
  repository at `forge.etsi.org`. Strongest source here; quotes are exact.
- [`iotagent-node-lib` API documentation](https://iotagent-node-lib.readthedocs.io/en/latest/api.html)
  — provisioning endpoints, attribute kinds, the NGSI surface.
- [Northbound interactions](https://github.com/telefonicaid/iotagent-node-lib/blob/master/doc/devel/northboundinteractions.md)
  — command status attributes and values; quotes are exact, typos included.
- [`iotagent-node-lib`](https://github.com/telefonicaid/iotagent-node-lib) — the
  payload/transport separation.
- Agent list: the FIWARE catalogue, plus
  [iotagent-opcua](https://github.com/FIWARE-GEs/iotagent-opcua) and
  [iotagent-isoxml](https://github.com/FIWARE/iotagent-isoxml).
