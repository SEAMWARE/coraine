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

Where the file comes from:

| | File read | When it is missing or unreadable |
|---|---|---|
| `--bridgeConfig <file>` (env `CORAINE_BRIDGECONFIG`) | `<file>` | the broker exits |
| no `--bridgeConfig` | `/opt/seamware/etc/bridges.json` - `$SEAMWARE_ETC_DIR/bridges.json` when `SEAMWARE_ETC_DIR` is set and not empty | no Channels from a file; not an error |

`SEAMWARE_ETC_DIR` moves the default file as `SEAMWARE_PLUGIN_DIR` moves the plugins
([Installation - environment variables](installation.md#environment-variables)). Neither the `coraine`
package nor `make install` installs a `bridges.json`. Each bridge's `init()` is given the file the
Channels came from, whether named or found by default, and NULL when there is none.

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

Today Bridges and Channels come **only** from this file - plus the Channels the broker makes on a
bridge's catch-all entity (`ngsild.defaultEntity`) for endpoints its transport discovers - and are
read-only over the API. No Channel is removed while the broker runs, so the broker never calls a
plugin's `channelDel`. Creating, changing and deleting them at runtime (`POST` / `PATCH` /
`DELETE`, stored in the database, 409 on a duplicate id) is the design, not yet built.

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
  arriving?"* is a `GET`, not a log hunt. A plugin can say so later, too, and
  why, in its own words (`channelStatusIn`, bridge ABI 12 -
  `COR_FEATURE_CHANNEL_STATUS_IN`): a resource its peer does not have, a peer
  that cannot be reached, a peer that is back.
- **What a Bridge carries** - `channels`, `dataChannels` and `assetChannels`:
  the ids of its Channels, DataChannels and AssetChannels in the request's
  tenant, so everything one Bridge does is one `GET`. A member only when it
  has one (`dataChannels` needs `COR_FEATURE_DATA_CHANNELS`, `assetChannels`
  `COR_FEATURE_ASSET_CHANNELS`).
- **Counters** - `samplesIn`, `samplesOut`, `requestsWaiting`,
  `requestsNotWaited`; on action Channels `goalsSent`, `goalCancelsSent`;
  `endpointDiscovered: true` once the transport found the endpoint (absent
  before, never `false`); `payloadsRefused` (the payload check, below). On the
  Bridge, `samplesDropped`. In memory, from the
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
- **Catch-all entity**, on unless the file says `"ngsild": { "defaultEntity": false }`:
  endpoints no Channel claims are stored on `urn:ngsi-ld:<bridge>:default`
  (for DDS `urn:ngsi-ld:dds:default`, type `DDS`), one attribute per endpoint,
  inbound only. The attribute's IRI is `@vocab` + the endpoint
  (`rt/chatter` → `…/default-context/rt/chatter`).
- **Overlap with registrations**: a mirroring Channel and an `exclusive` or
  `redirect` registration may not claim the same attribute; `inclusive` and
  `auxiliary` never conflict.

## The payload check

A bridge may refuse what a client writes to an attribute one of its Channels
or DataChannels maps, before anything of the write is sent or stored
(`BridgeDriver.payloadCheck`, bridge ABI 13 - `COR_FEATURE_CHANNEL_CHECKS`):

- **Refuse only.** The bridge is handed the endpoint, the attribute (expanded)
  and the value as the client wrote it (the default instance, as the
  application's own JSON - on a merge, the fragment), and answers yes or no.
  A refusal is 400 (`BadRequestData`, the bridge's reason in `detail`); a
  check that fails is a refusal too, 503 (the bridge could not check the
  value). Only the attribute is refused, never the write around it: a write of
  that attribute alone fails and nothing is written; in a multi-attribute write
  or a batch that attribute is left out and reported, and the other attributes
  and entities are written (207). The value is never changed.
- **Optional, per bridge.** A bridge without one costs a write one test of a
  flag. A transport that refuses a wrong value itself (DDS) needs none - a
  Channel writes transport first.
- **Which attributes.** A Channel's own; for a DataChannel, the attributes its
  mapping names, of every entity whose id fits one of its id templates
  (`urn:ngsi-ld:BikeStation:{Number}` fits `urn:ngsi-ld:BikeStation:7`). An
  AssetChannel's are not checked yet.
- **Clients only.** What the bridge itself writes - a sample, a DataChannel's
  record - is not checked.
- **`payloadsRefused`**, on the Channel and the DataChannel: the writes its
  bridge refused.

The loopback bridge's `"payloadCheck"` (`{ "<endpoint>": "number" |
"refuse:<reason>" | "error" }`) is the reference implementation.

## DataChannels

A Channel is one endpoint, one attribute of one entity. A **DataChannel** is
one endpoint whose samples are **records** - a row of a CKAN resource, a line
of a CSV file: a JSON object, one member per column - and each record becomes
one or more entities, the entity id taken from a column. It is not a Channel:
it names no entity and no attribute, claims none, and has its own listing.
Inbound only. In the file, a bridge's `dataChannels` section, one entry per
endpoint:

```json
{ "ckan": { "server": { "url": "https://data.boston.gov" },
            "ngsild": { "dataChannels": {
              "44c931aa-4577-4caa-a8f2-ed1e38263e70": {
                "channelInfo": [ { "key": "pageSize", "value": "100" } ],
                "idEncoding": "merge",
                "entities": [
                  { "type": "BikeStation", "id": "urn:ngsi-ld:BikeStation:{Number}",
                    "attributes": {
                      "name":       { "column": "Name" },
                      "totalDocks": { "column": "Total_docks", "as": "number" },
                      "location":   { "point": { "longitude": "Longitude", "latitude": "Latitude" } },
                      "inDistrict": { "relationship": "urn:ngsi-ld:District:{District}" } } },
                  { "type": "District", "id": "urn:ngsi-ld:District:{District}",
                    "attributes": { "name": { "column": "District" } } } ] } } } } }
```

- **`id`** is a template: `{Column}` is the column's value. A record with no
  value in a column the id names makes no entity of that kind; the others it
  names are still made.
- **`idEncoding`** - how a value goes into an id or a Relationship's object:
  - `merge` (the default): each character outside `[A-Za-z0-9._-]` becomes
    `_` (`South Boston` -> `South_Boston`). Values that differ only there name
    one entity, and their records are merged into it.
  - `percent`: each byte outside `[A-Za-z0-9._-]` becomes `%XX`
    (`South Boston` -> `South%20Boston`). Two different values never name the
    same entity.
- **`attributes`**: `{ "column": ... }` a Property, `"as"` `number`, `boolean`
  (`yes`/`true`/`y`/`1`, `no`/`false`/`n`/`0`) or `string` converting the
  value; `{ "point": { "longitude": ..., "latitude": ... } }` a GeoProperty
  Point; `{ "relationship": <template> }` a Relationship. An absent, null or
  empty column, or a value that does not convert, leaves the attribute out.
  Names and types are expanded as a Channel's are.
- **The write** is one batch upsert per record, `options=update`, local: the
  entities are created when they are not there, their mapped attributes
  replaced, other attributes left alone - with the checks, notifications and
  temporal events of `POST /entityOperations/upsert`, each entity on its own:
  one the broker refuses does not take the record's others with it.
- **`observedAt`** is the record's time, when its bridge gives one (CKAN: the
  `timeColumn`); without one, no `observedAt`.
- **No owner.** A DataChannel claims no attribute: two DataChannels, or a
  DataChannel and a Channel, may write the same attribute, and the last write
  wins (TRoE keeps the earlier values). One endpoint is a Channel's or a
  DataChannel's, never both - naming it as both is fatal at start.
- **`GET /ngsi-ld/v1/dataChannels`** (and `/{id}`, the id
  `urn:ngsi-ld:DataChannel:<bridge>:<endpoint>`): the bridge, the endpoint,
  `channelInfo`, `idEncoding`, the mapping as the file wrote it
  (`recordEntities`), `status` and `statusReason` (a bridge reports them with
  `channelStatusIn`), and `recordsIn` (records that wrote an entity),
  `recordsRefused` (not an object, no entity id, every entity refused) and
  `entitiesWritten`.

`COR_FEATURE_DATA_CHANNELS`; without it, a `dataChannels` section is warned
about and ignored, and `GET /dataChannels` answers 501. The first bridge built
on it is CKAN's (`ckan.so`, `corCkanBridge`).

## AssetChannels

A source whose data is a **model** - an Asset Administration Shell and its submodels, an OPC UA object and
its components - is neither a Channel nor a DataChannel: one asset is a tree with types, meaning and
references, and becomes a graph of entities, with Relationships where the model has references.

```json
{ "opcua": { "ngsild": { "assetChannels": {
    "ns=2;s=Press17": { "entities": "natural", "attributeLayout": "attributes" } } } } }
```

The bridge hands the model over on the endpoint as a neutral node tree, `{ "model": ... }`, then
`{ "values": [ ... ] }` as values change; which nodes become entities (`entities`: `"natural"` or rules - no
default), what a container becomes (`attributeLayout`), where a node's meaning goes (a `semanticId`
sub-Property), the ids, and the write (a DataChannel's batch upsert) are in
[asset-channels.md](asset-channels.md). `GET /ngsi-ld/v1/assetChannels` (and `/{id}`, the id
`urn:ngsi-ld:AssetChannel:<bridge>:<endpoint>`) lists them. `COR_FEATURE_ASSET_CHANNELS`; without it, an
`assetChannels` section is warned about and ignored, and `GET /assetChannels` answers 501. One endpoint is
a Channel's, a DataChannel's or an AssetChannel's, never two of them.

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
