# Bridges, channels and Service Execution

> **Design - not built.** For review. What it rests on is built: [Bridges and Channels](bridge-channels.md)
> (Channel, DataChannel, AssetChannel) and [Service Execution](service-execution.md), with its bridge
> executor (bridge ABI 11). The designs it ties together: [IoT Agents as Service Executors](iot-agent-executor.md),
> [IoT Agent measures as DataChannel records](iot-agent-measures.md), [AssetChannels](asset-channels.md) § 4.

A device, a robot or an asset is reached through a Bridge. What a client does with it is one of three
things, and each has one mechanism:

| The client… | Mechanism | Examples | Answer |
|---|---|---|---|
| **changes state** - writes an attribute | a channel, **outbound**: transport first, the entity mirrors the device | a lamp's `on`, a setpoint, a Modbus register, a KNX group address, an OPC UA variable | the write's own: 204, or the refusal (400, 503) and nothing stored |
| **asks for something to be done** | a **Service Execution**; the Bridge is its executor | an IoT Agent command, an OPC UA Method, an AAS Operation, a DDS service or action, a Hue scene | an execution: status, progress, output, error, cancel |
| **reads a value that lives at the device** | a **registration** whose endpoint names a bridge scheme; the broker forwards the read | a lazy attribute: a battery level that polling would drain | the forwarded query's |

A state change has no status, no output and nothing to cancel; an execution changes no attribute by
itself - what the device does as a result comes back as a sample, on a channel, like any other change of
its state. Neither is expressed through the other.

## 1. What is built

- **Channel**: inbound, outbound or both. An outbound write is sent first (`BridgeDriver.publish`) and
  stored only when the transport takes it.
- **DataChannel, AssetChannel**: inbound only.
- **The bridge executor**: a Service Registration whose endpoint has a scheme a loaded plugin claims
  (`serviceSchemes`) is executed by the plugin (`serviceExecute`, `serviceCancel`,
  `BridgeBroker.serviceUpdateIn`). Only the loopback bridge implements it.
- **DDS services and actions**: Channels of kind `service` and `action`, in the convention
  [bridge-channels-details.md § 10](bridge-channels-details.md#10-service-execution-a-provisional-convention-quarantined)
  calls provisional until Service Execution exists: the reply in the attribute, goals as instances
  (`datasetId urn:goal:<id>`), `/ngsi-ld/v1/channels/{id}/goals`, `?ddsSync`.

## 2. Write-back for DataChannels and AssetChannels

Most of a home is controlled by writing state (`on`, brightness, a thermostat's setpoint), and so are
OPC UA Write, BACnet WriteProperty and LWM2M Write. A model or record source has no Channel per attribute,
so without write-back such a write either needs a Channel per attribute - defeating the mapping - or a
service per writable attribute.

**The design:** a DataChannel or AssetChannel may be outbound too, with a Channel's rule.

- **Opt-in, per channel**: `channelDirection` - `in` (the default, as today) or `both`. A member of the
  shared base ([asset-channels.md § 2](asset-channels.md#2-what-the-three-share)), so every channel type
  has it.
- **Which attributes**: those the payload check already covers - for a DataChannel the attributes its
  mapping names, of an entity whose id fits one of its templates; for an AssetChannel those its last model
  made.
- **Transport first**: the bridge is handed the write before anything is stored; a refusal stores nothing
  (400, 503), in a batch that attribute is left out (207). The payload check, when the bridge has one,
  runs before it.
- **No new bridge entry for the write**: `publish(endpoint, json)`, as for a Channel. The plugin knows
  each endpoint's channel type from `channelTypedAdd` (bridge ABI 15).
  - **AssetChannel**: the endpoint is the attribute's node (`ns=2;s=P17.Pressure`), as the payload check
    is handed it; the json the value.
  - **DataChannel**: the endpoint is the DataChannel's; the json a record fragment - the columns the
    entity's id template names, read back out of the id, and the written attribute's column:
    `urn:ngsi-ld:Lamp:{deviceId}` written `on: true` -> `{ "deviceId": "lamp1", "state": true }`. The
    plugin addresses the device from it (zigbee2mqtt: `zigbee2mqtt/lamp1/set`).
- **An id read back** needs an id template that can be inverted: `idEncoding: percent`. `merge` is lossy
  (`South Boston` and `South_Boston` name one entity), so a DataChannel with `channelDirection: both`
  and `idEncoding: merge` is refused at start.
- `COR_FEATURE_CHANNEL_WRITE_BACK`, sized before it becomes a declared switch.

## 3. DDS services and actions as Service Executions

Two forms of one thing now exist:

| | DDS Channel (built) | Service Execution |
|---|---|---|
| invoke | write the attribute | `POST /entities/{id}/services/{name}` |
| wait for the answer | `?ddsSync=true`, `--ddsSyncTimeout` (200 ms) | `mode: synchronous`, `executionTimeout` |
| goals | instances `urn:goal:<id>`, `/channels/{id}/goals` | executions, `/services` |
| feedback, result | `ddsActionFeedback`, `reply` sub-Properties | `executionProgress`, `executionOutput` |
| rejected goal | 422 `goalRejected` | the executor refuses: 502 |
| cancel | `DELETE /channels/{id}/goals/{goalId}` | `DELETE /services/{id}` |

The Channel form is Orion-LD's, and DDS stays shaped as Orion-LD's until the ARISE project ends: seen
as a black box, the broker behaves as Orion-LD does - the same configuration file, the same default user
@context at startup, the same entities and attributes, with only the deliberate differences
[DDS in coraine](dds.md) lists. Nothing in this note changes what a DDS deployment configured that way
sees (`bridge_default_user_context.test`).

**Proposal: both faces, one executor.** The DDS plugin also claims `dds://`; a Service Registration
`dds://<service or action name>` makes a DDS service or action a service of the entity. Its
executions use the plugin's existing calls (`serviceInvokeTracked`, `actionGoalSend`,
`actionGoalCancel`), the answer reported through `serviceUpdateIn`. The Channel form stays as it is,
unchanged; after ARISE it can be deprecated. An execution writes no attribute; the Channel form keeps
writing its own.

## 4. Service Registrations a bridge makes

An AssetChannel registers each Operation and Method it maps; drop-in IoT Agent provisioning
(`POST /iot/devices`) registers each command. Those registrations follow the rule of the channels they
come from ([Bridges and Channels](bridge-channels.md#configuring), decided 2026-10-20):

- **In memory, not in the database**: made at start from the configuration, and again when a model is
  sent again; gone after a restart when their source no longer names them. Nothing to find and delete.
- **Shown with the stored ones**: `GET /ngsi-ld/v1/serviceRegistrations` lists them beside the
  registrations clients created, and invoking one is the same as invoking any other.
- **Their id** is the bridge's - `urn:ngsi-ld:ServiceRegistration:<bridge>:<endpoint>:<serviceName>` -
  so a model sent again replaces them, and never trips the one-name-per-entity rule (409) against itself.
- **`PATCH` and `DELETE`** change them in memory, until the restart or the next model rebuilds them.
- **A client's registration clashing with one** (same id, or the same service name on an entity both
  select): the client's create is refused, 409, as today. At start, a bridge's clashing with a stored
  one: the broker does not start and names both - one endpoint is one channel's, never two.
- **In the channel's tenant.**
- **A node the model no longer has** keeps its registration until the restart, as its entity stays
  (asset-channels.md § 6.8); an execution of it fails, 503 ("has no such service"), until the node is
  back.
- **Marked as the bridge's**: `bridgeId` on the registration, the Bridge's id. Not in GR CIM-055; shown
  only on registrations a bridge made.

A registration a client creates with a bridge scheme (the native path of the IoT Agent executor) is the
client's, stored as any other.

## 5. Which Bridge executes

The endpoint's scheme selects the plugin. A plugin carries one Bridge today (the file cannot name a
second), so the scheme is enough. When a plugin carries more than one, the endpoint names the Bridge:
`?bridge=<name>`, defaulting to the plugin's only one - additive, and an endpoint written today keeps
its meaning.

## 6. What a Bridge lists

A Bridge lists what it carries - `channels`, `dataChannels`, `assetChannels` - and adds **`services`**:
the ids of the Service Registrations it executes, in the request's tenant. Everything one Bridge does
stays one `GET`.

A Bridge that is `unavailable` executes nothing: an execution fails at once, 503, and the Bridge's
`status` says why. The `ServiceDescription` an entity shows (`includeServices=true`) is GR CIM-055's and
gets no status member.

## 7. Across bridges

Nothing more is needed: a grouped execution (`switchOff`, `entityType: Lamp`, `q: floor==3`) runs on each
selected entity's own registration, so lamps behind a Hue bridge and behind zigbee2mqtt are switched by
one request; a combined execution sequences services of different bridges.

## 8. Open decisions

1. **DDS** (§ 3): both faces with one executor, or the Channel form alone until ARISE ends. Either way
   the Orion-LD behaviour stays as it is.
2. **Write-back** (§ 2): `channelDirection` on the base; refuse `merge` with `both`, or let the plugin
   keep its own device lookup.
3. **Bridge-made registrations** (§ 4): `bridgeId` as a member, or nothing on the registration and the
   Bridge's `services` list alone.
4. **The Bridge in the endpoint** (§ 5): `?bridge=`, or wait until a plugin carries two Bridges.

## 9. Not in this note

- Persisting what pairing produces (a Hue application key, Matter fabric credentials): a Bridge has no
  store of its own today.
- Stateless events (a button press): two equal presses in a row are equal values; the bridge gives each
  sample its time, so each is a change.
- The provisioning API and channel CRUD over the API: designed in their own notes, not built.

## 10. Order

1. Write-back (§ 2) - the gap that blocks control of the smart-home and model sources.
2. The Bridge's `services` and bridge-made registrations (§ 4, § 6) - with the AssetChannel's operations
   (asset-channels.md § 4) and the IoT Agent executor, whichever comes first.
3. DDS as an executor (§ 3), once decided.
