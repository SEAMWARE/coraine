# Bridges and Channels

A **Channel** ties one endpoint of a foreign transport - an MQTT topic, a DDS
topic, service or action, a Modbus register - to one attribute of one NGSI-LD
entity. A **Bridge** is the transport instance that carries Channels: one DDS
participant, one MQTT connection. A value that arrives on the endpoint becomes
the attribute's value; a write to the attribute goes out on the endpoint.

> Not standard NGSI-LD. coraine implements this now; the concept goes to the
> ETSI TC DATA face-to-face in Athens, 20–22 October 2026, and anything
> normative will realistically follow in 2027. The names may change; the
> endpoint-scheme convention is the part most likely to survive.

## Three concepts, three clocks

| | What it is | Changes at | Over the API |
|---|---|---|---|
| **Capability** | a bridge plugin (`dds.so`, `mqtt.so`, `modbus.so`), named on `--bridges` | startup | not an object |
| **Bridge** | one transport instance - a DDS domain, an MQTT server | configuration | `GET /ngsi-ld/v1/bridges` |
| **Channel** | *this endpoint ↔ this attribute, this direction* | configuration | `GET /ngsi-ld/v1/channels` |

`--bridges dds` makes the broker *able* to speak DDS; it creates no Bridge.
Plugins are never loaded at runtime and never inferred from configuration.

**A Channel is not a registration.** A registration says *another context
source holds this value; forward to it*. A Channel says *the broker holds the
value and a foreign wire carries a copy*. A value fetched only on demand (a
battery level that polling would drain) stays a registration.

## Configuring

```
coraine --bridges mqtt --bridgeConfig /etc/coraine/bridges.json
```

`--bridgeConfig` is one JSON file: per plugin, the transport's own settings and
an `ngsild` section mapping endpoints to `(entityId, entityType, attribute)`.
For DDS it is the Orion-LD configuration file, unchanged - `dds.ddsmodule`
(eProsima's, passed verbatim) becomes the Bridge, `dds.ngsild.topics` /
`.services` / `.actions` become Channels. An MQTT example:

```json
{ "mqtt": { "server": { "uri": "mqtt://mosquitto:1883", "MQTT-Version": "mqtt5.0" },
            "ngsild": { "topics": {
              "plant/tank1/level": { "entityId": "urn:ngsi-ld:Tank:T1", "entityType": "Tank",
                                     "attribute": "level" } } } } }
```

Anything the file does not say defaults to Orion-LD's behaviour: direction
`both`, retention `mirror` (stored locally *and* put on the wire). Every
configured attribute exists from startup, with the value `"uninitialized"`
until the first sample.

Today Bridges and Channels come **only** from this file and are read-only over
the API. Creating and changing them at runtime (`POST` / `PATCH` / `DELETE`,
stored in the database, 409 on a duplicate id) is the design, not yet built.

## What `GET` shows

```json
{ "id": "urn:ngsi-ld:Channel:loopback:P1", "type": "Channel",
  "bridgeId": "urn:ngsi-ld:ContextBridge:loopback",
  "channelKind": "topic", "channelDirection": "both", "channelTarget": "P1",
  "entity": { "id": "urn:M:1", "type": "Machine" }, "entityAttribute": "temperature",
  "retention": "mirror", "status": "available",
  "samplesIn": 0, "samplesOut": 0, "requestsWaiting": 0, "requestsNotWaited": 0 }
```

- **`status`** - a Bridge is `available`, or `unavailable` when its plugin
  was not named on `--bridges`; a Channel is `available`, or `dormant` when it
  cannot carry (its Bridge is unavailable, or the transport refused it). A
  degraded object never stops the broker from starting; *"why is nothing
  arriving?"* is a `GET`, not a log hunt.
- **Counters** - `samplesIn`, `samplesOut`, `requestsWaiting`,
  `requestsNotWaited`; on action Channels `goalsSent`, `goalCancelsSent`;
  `endpointDiscovered: true` once the transport found the endpoint (absent
  before, never `false`). On the Bridge, `samplesDropped`. In memory, from the
  broker's start. Tests wait on these, never on a trace line.

## Behaviour

- **Transport first.** A write to an attribute an outbound Channel carries is
  sent BEFORE anything is stored, and a refusal stores nothing: 400 for a
  value that does not fit the topic's type, 503 for a topic not yet announced,
  422 (`goalRejected`) for a rejected goal, 504 for an unanswered one (and the
  goal is cancelled). In a multi-attribute write or a batch the refused part
  is left out and the rest written (207). The entity mirrors the wire.
- **Services** (DDS, broker as client): the write invokes; the reply lands in
  the same attribute. `?ddsSync=true` (or `--ddsSync`) waits for the reply,
  up to `--ddsSyncTimeout` (default 200 ms, or the file's `syncTimeoutMs`).
- **Actions** (DDS, broker as client): each goal is an instance of the
  attribute (`datasetId urn:goal:<id>`) with its feedback, status and result.
  Goals are also served at `/ngsi-ld/v1/channels/{id}/goals`
  (`POST`, `GET`, `GET …/{goalId}`, `DELETE …/{goalId}` to cancel). Progress is
  notified as an ordinary NGSI-LD `Notification` to the goal's own endpoint,
  else the Channel's default, else the Bridge's. This representation is
  provisional until ETSI's Service Execution lands.
- **No echo.** A sample that arrived on a transport is not published back out
  of it.
- **Catch-all entity**, off unless asked (`"ngsild": { "defaultEntity": true }`):
  endpoints no Channel claims are stored on `urn:ngsi-ld:<bridge>:default`
  (for DDS `urn:ngsi-ld:dds:default`, type `DDS`), one attribute per endpoint,
  inbound only. The attribute's IRI is `@vocab` + the endpoint
  (`rt/chatter` → `…/default-context/rt/chatter`).
- **Overlap with registrations**: a mirroring Channel and an `exclusive` or
  `redirect` registration may not claim the same attribute; `inclusive` and
  `auxiliary` never conflict.

## Key limits

- Client side only: the broker never answers a DDS request or executes a goal.
- One Bridge per plugin from the file (the format cannot name a second).
- An MQTT Channel's topic is exact - no `+` / `#` wildcards.
- The plugin contract is `corBridge/BridgeDriver.h`: append-only, with an ABI
  version handshake; a feature compiled out of the broker is a NULL pointer,
  never a `#if` inside the struct.
- Each bridge is its own repository (`corDdsBridge`, `corMqttBridge`,
  `corModbusBridge`); the `.so` installs as
  `/opt/seamware/plugins/bridge/<name>.so`.

Per transport: [DDS](dds.md), [MQTT](mqtt-bridge.md), [Modbus](modbus-bridge.md).

The full reference: [bridge-channels-details.md](bridge-channels-details.md).
How it came about: [history](history/bridge-channels.md).
