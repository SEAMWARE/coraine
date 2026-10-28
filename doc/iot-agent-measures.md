# IoT Agent measures as DataChannel records - design note

**Status: designed, decisions taken (KZ, 2026-10-20, § 5). Built in the broker: the DataChannel additions a-d and f, and the per-bridge `otherColumns` ([DataChannels](bridge-channels.md#datachannels)); the plugin's measures are next.** Measures are the second job of an IoT Agent: the data a device sends
up. Commands are the first, and are Service Executions ([IoT Agents as Service Executors](iot-agent-executor.md)).
Lazy attributes, the third, stay a Context Source Registration (decided 2026-10-14).

The design: **a measure is a record, and a DataChannel makes records into entities.** The plugin hands
the broker `(endpoint, record, time)` and knows nothing about entities. The broker's DataChannel maps the
record onto the device's entity and upserts it.

## 1. What iotagent-json does with a measure

Sources: `telefonicaid/iotagent-json` 736b443 - `docs/usermanual.md`, `lib/bindings/*.js`,
`iotagent-node-lib/doc/api.md`, `iotagent-node-lib/lib/services/ngsi/entities-NGSI-LD.js`.

### 1.1 How a device sends one

| Transport | Where | The measure |
|---|---|---|
| MQTT | `[/json]/<apikey>/<deviceId>/attrs` | a flat JSON object, `{"t": 21, "h": 40}`; or an array of them |
| MQTT | `[/json]/<apikey>/<deviceId>/attrs/<name>` | one value, the name in the topic; not JSON: binary, stored as hex |
| HTTP | `POST /iot/json?k=<apikey>&i=<deviceId>[&t=<time>]` | a flat JSON object, or an array |
| HTTP | `POST /iot/json/attrs/<name>?k&i` | one value: JSON, `text/plain`, `application/octet-stream` (hex) or SOAP XML |
| HTTP | `GET /iot/json?k&i&d=<json>` | the object in `d` |

An HTTP measure is answered `200 {}`; with `getCmd=1` the answer carries the device's pull commands.

### 1.2 Groups and devices

- A **config group** is one `(apikey, resource)`. It holds the entity type, the attribute mapping, the
  static attributes, `timestamp`, `explicitAttrs`, `autoprovision`, and the service and subservice
  (tenant, path).
- A **device** is `(service, subservice, device_id, apikey)`. It may override the group: its own
  `entity_name`, `entity_type`, mapping and flags. A device inherits whatever it does not set.
- **The entity id** is `<entity_type>:<device_id>` unless the device names one, or the group has an
  `entityNameExp` (JEXL).
- **`autoprovision`** (default `true`): a measure from a device not provisioned creates the device, and
  its entity, from the group. With `false` the measure is refused.

### 1.3 The mapping

- Each `attributes` entry: `object_id` (the key the device sends), `name` and `type` (the attribute),
  optional `metadata` (`unitCode` and others), `expression` (JEXL), `skipValue`, and `entity_name` /
  `entity_type` (the value goes to another entity: "multientity").
- **A key no entry maps** becomes an attribute of the same name, unless `explicitAttrs` is set, which
  drops it.
- **`static_attributes`** go into every write as they are.
- A measure named `id` or `type` becomes `measure_id` / `measure_type`.

### 1.4 The NGSI-LD write

- One `POST /ngsi-ld/v1/entityOperations/upsert?options=update` per measure. A multi-measure array is
  one entity per element, sorted by `TimeInstant` (ascending).
- An attribute's `type` becomes the NGSI-LD attribute:

| `type` (case-insensitive) | NGSI-LD |
|---|---|
| `Property`, `Text`, `String`, anything else | Property, the value as sent |
| `Number` / `Integer` / `Float` | Property, parsed as a number; not a number: left out |
| `Boolean` | Property, `!!value` |
| `Object` / `Array` | Property, the value JSON-parsed when it is a string |
| `DateTime` / `Date` / `Time` | Property, normalised (ISO 8601, UTC) |
| `geo:point`, `Point`, `GeoProperty`, `geo:json`, `geo:linestring`, `geo:polygon`, `geo:multi*` | GeoProperty: a `"lat, lng"` string or GeoJSON made into GeoJSON |
| `Relationship` / `ListRelationship` | Relationship `object` / `listObject` |
| `LanguageProperty`, `VocabProperty`, `JsonProperty`, `ListProperty` | that type, the value in its member |

- A `null` or `NaN` value leaves the attribute out.
- **`observedAt`** on each attribute: the measure's `TimeInstant` member when it is ISO 8601, else the
  `t` parameter, else the time the measure arrived. `timestamp: false` does not remove it in practice:
  `api.md`'s table gives the same behaviour for `true`, `false` and not set.
- `unitCode` metadata becomes the attribute's `unitCode`; other metadata become sub-attributes.

## 2. The design

### 2.1 Why a DataChannel

| | Fits a measure? |
|---|---|
| **A Channel** - one endpoint, one attribute of one entity | No: a device sending five keys needs five Channels, and a device not provisioned has none (no autoprovision) |
| **The plugin writes the entity** | No: a bridge knows nothing about entities (the bridge seam: `(endpoint, json, time)`); the mapping, the types and the write would be the plugin's, once per IoT Agent |
| **A DataChannel** - one endpoint whose samples are records, each made into entities by a mapping | Yes: a measure *is* a record (a JSON object, one member per key); the id comes from a template; the write is the upsert iotagent does, `options=update` |

So the IoT Agent JSON is a bridge whose DataChannels are its config groups and devices. The other agents
(UltraLight, LoRaWAN, ...) then differ only in how they read a measure off the wire.

### 2.2 Endpoints

| DataChannel endpoint | iotagent-json | Takes the measures of |
|---|---|---|
| `<apikey>` | a config group | every device of that apikey without a DataChannel of its own |
| `<apikey>/<deviceId>` | a provisioned device | that device |

The broker hands the plugin its DataChannels (`channelAddInfo`), so the plugin knows which endpoints have
one. A measure goes to the device's DataChannel when there is one, else the group's. **With neither**
the measure is refused: dropped with a warning on MQTT, an error on HTTP. That is `autoprovision: false`
for an apikey with no group DataChannel, and `true` for one that has it.

The HTTP error is iotagent-json's - its status and `name`, so a device or a tool that checks them sees
no difference; the `message` is ours:

| The apikey | 404, `name` |
|---|---|
| no DataChannel at all - no group | `DEVICE_GROUP_NOT_FOUND` |
| device DataChannels, no group DataChannel (`autoprovision: false`), and not this device | `DEVICE_NOT_FOUND` |

The body is `{ "name", "message" }`, as iotagent-json's (`HTTPBinding.js` `handleError`).

The bridge carries **DataChannels only**: a plain Channel on it (`<apikey>/<deviceId>/<key>` to one
attribute) is refused at start. Allowing them later is additive.

### 2.3 The record and its time

- **The record** is the measure object as the device sent it, plus the device id and the apikey as
  the columns **`@deviceId`** and **`@apikey`**, so that the id template can name them:
  `"id": "urn:ngsi-ld:Thermometer:{@deviceId}"`. No device key collides with them, and no @context
  can name an attribute so: JSON-LD 1.1 reserves every term of the form `@` followed by letters, and
  ignores one that a context defines. They never become attributes, `otherColumns` or not.
- `id` / `type` keys renamed `measure_id` / `measure_type`, as iotagent-json does.
- **The time** (the record's `publishTime`, and so every attribute's `observedAt`): the `TimeInstant`
  member (ISO 8601; removed from the record), else `t`, else the arrival time - iotagent-json's order.
- **A multi-measure array** is one record per element, in ascending `TimeInstant` order.
- **A single measure** (`.../attrs/<name>`) is the record `{"<name>": <value>}`.
- The tenant is the DataChannel's (iotagent-json's `service`). The subservice (`fiware-servicepath`) is
  the entity's **scope** - NGSI-LD's version of NGSIv2's Service-Path: `/garden/north` is a scope as it
  is - set in the DataChannel's entity mapping (`"scope"`, addition f). No subservice, no scope.

### 2.4 What DataChannels need first

DataChannels today: `column` (Property, `as` `number` / `boolean` / `string`), `point` from two columns,
`relationship` from a template; the mapped columns only; one batch upsert per record. A measure needs:

| | iotagent-json | The DataChannel addition |
|---|---|---|
| a. | unmapped keys become attributes (`explicitAttrs: false`, the default) | `"otherColumns": "property"` - every column the mapping does not name becomes a Property of that name; absent: dropped, as today (`explicitAttrs: true`) |
| b. | the attribute types of § 1.4 | `as`: `integer`, `float`, `datetime`, `json`; `geo` from one column (`"lat, lng"` or GeoJSON); `languageMap`, `vocab`, `json` |
| c. | `static_attributes` | `{ "value": <constant> }` |
| d. | `unitCode` metadata | `"unitCode"` on an attribute's mapping |
| e. | the device id in the entity id | the reserved columns of § 2.3 - the plugin's part; nothing in the broker |
| f. | the subservice | `"scope"` on an entity's mapping: a scope, or a template (`/{District}`), the entity's `scope` |

**`otherColumns` per bridge**: `"otherColumns"` in the bridge's `ngsild` section is the default for every
DataChannel of the bridge, and a DataChannel's own replaces it - the IoT Agent bridge's file says
`property` once, the CKAN bridge's says nothing and keeps the mapped columns only. Columns starting
with `@` are never other columns.

a-d and f are additions to the broker's DataChannel mapping, each useful to the CKAN bridge too. All are
additive: a mapping that does not use them reads as today.

### 2.5 Not in this step

- **JEXL** - `expression`, `entityNameExp`, `explicitAttrs` as an expression. The costly part of
  node-lib, and a language of its own: a later decision, perhaps a subset.
- **Multientity** (`entity_name` per attribute). A DataChannel already makes several entities of one
  record; per-attribute target entities, later.
- **`payloadType` `ngsiv2` / `ngsild`**, binary (hex) and SOAP XML measures.
- **The provisioning API** (`/iot/groups`, `/iot/devices`): its own PR (decision 3 of the commands note).
  It creates DataChannels at runtime, which needs DataChannel CRUD over the API - designed, not built.
  Until then groups and devices come from `--bridgeConfig`.

## 3. An example

```json
{ "iotajson": {
    "server":     { "uri": "mqtt://mosquitto:1883" },
    "southbound": { "port": 7896 },
    "ngsild": {
      "otherColumns": "property",
      "dataChannels": {
        "thermo-key": {
          "entities": [ { "type": "Thermometer", "id": "urn:ngsi-ld:Thermometer:{@deviceId}",
                          "scope": "/garden/north",
                          "attributes": {
                            "temperature": { "column": "t", "as": "number", "unitCode": "CEL" },
                            "location":    { "geo": "loc" } } } ] } } } } }
```

A device `th1` publishes `{"t": "21.5", "h": 40, "loc": "40.4, -3.7"}` on `/json/thermo-key/th1/attrs`.
The broker upserts:

```json
{ "id": "urn:ngsi-ld:Thermometer:th1", "type": "Thermometer", "scope": "/garden/north",
  "temperature": { "type": "Property", "value": 21.5, "unitCode": "CEL", "observedAt": "<arrival>" },
  "location":    { "type": "GeoProperty", "value": { "type": "Point", "coordinates": [-3.7, 40.4] },
                   "observedAt": "<arrival>" },
  "h":           { "type": "Property", "value": 40, "observedAt": "<arrival>" } }
```

## 4. The first PRs, if this holds

1. **coraine**: the DataChannel additions a-d and f, and the per-bridge `otherColumns`, each with its
   functests on the loopback bridge.
2. **[corIotaJsonBridge](https://github.com/SEAMWARE/corIotaJsonBridge)**: MQTT measures (`[/json]/+/+/attrs[/+]`) and HTTP measures on the southbound
   port; the plugin takes DataChannels (it refuses Channels today); the record, its time, arrays.
   Its tests against the broker of 1.

## 5. Decisions (KZ, 2026-10-20)

1. **The reserved columns**: `@deviceId` and `@apikey` - no device sends such a key, and no @context
   has such a term (§ 2.3).
2. **`otherColumns`**: a per-bridge default, a DataChannel's own replacing it (§ 2.4) - as the DDS
   bridge's catch-all entity takes what nothing else claims.
3. **A measure with no DataChannel on HTTP**: iotagent-json's 404 and `name` - `DEVICE_GROUP_NOT_FOUND`,
   `DEVICE_NOT_FOUND` - our own `message` (§ 2.2).
4. **Plain Channels on this bridge**: no - DataChannels only (§ 2.2).
5. **The subservice**: the entity's `scope` (§ 2.3, addition f).
