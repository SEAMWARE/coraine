# AssetChannels - a source model as a graph of entities

**Status:** built in the broker (§ 7 steps 1-3): the Bridge listing what it carries, the payload check (§ 2.1, [bridge-channels.md](bridge-channels.md#the-payload-check)), and the AssetChannel - its object, its listing, the mapping (§ 3) and the node tree a bridge hands over (§ 3.3), `COR_FEATURE_ASSET_CHANNELS`. The OPC UA bridge (§ 7 step 4) is [corOpcuaBridge](https://github.com/SEAMWARE/corOpcuaBridge). Not built: deleting what a model no longer has, operations as services (§ 4).

A [Channel](bridge-channels.md) ties one endpoint to one attribute; a [DataChannel](bridge-channels.md#datachannels)
makes each record of an endpoint one or more entities. Neither fits a source whose data is a **model**: an
Asset Administration Shell with its submodels, an OPC UA server's objects. There one asset is a tree of
elements, typed and with meaning attached (a semanticId, a type definition), with references across it -
and it becomes a **graph of entities**, with Relationships where the model has references. An AssetChannel
is that mapping.

## 1. Three shapes of data, three channel types

| | One message, one reading | Becomes | Type |
|---|---|---|---|
| **Signal** | one value: a DDS topic sample, an OPC UA variable, a Modbus register | one attribute of one entity | Channel |
| **Record** | a flat object of fields: MQTT JSON (an IoT agent's measures), a Kafka message, a CKAN row | an entity or a few, a field per attribute | DataChannel |
| **Model** | a tree with types, meaning and references: an AAS shell and its submodels, an OPC UA object and its components | a graph of entities and Relationships | **AssetChannel** |

Three types, not one with a sub-type: their members differ (an attribute and an entity; a column mapping
and an id template; a model root and node rules), and a type whose members depend on a sub-type is what
OpenAPI describes worst - and what every client must branch on (decided 2026-10-16).

## 2. What the three share

Every channel type is the same base, and adds its own members:

| Member | |
|---|---|
| `id`, `type` | `urn:ngsi-ld:<Type>:<bridge>:<endpoint>`; `Channel`, `DataChannel`, `AssetChannel` |
| `bridgeId` | the Bridge that carries it - every channel points to its Bridge |
| `channelTarget` | the endpoint the Bridge reads: a topic, a register, a resource id, a model root |
| `channelInfo` | the endpoint's settings, the bridge's own (key, value) |
| `status`, `statusReason` | `available`, `dormant`, and why (`channelStatusIn`) |
| counters | what came in, what was refused - each type its own |

In OpenAPI: one base schema, each type `allOf` it. The Bridge lists what it carries - its Channels,
DataChannels and AssetChannels - so "everything this bridge does" is one request without one polymorphic
collection. (Channel and DataChannel have these members today - `channelTarget` both - the base makes it a
rule for every type.)

### 2.1 The payload check

A bridge may check what a client writes to an attribute one of its channels maps - Channel, DataChannel
and AssetChannel alike - before the broker stores it:

- **Refuse only.** The check answers yes or no; a no stores nothing, and the client gets 400 with the
  bridge's reason. It never changes the payload.
- **Optional, per bridge.** A bridge without a check costs nothing. One whose transport already refuses a
  wrong value (DDS: the DDS enabler rejects a sample that is not exact; a Channel writes transport first)
  does not need one - the check would be the same work twice, and throughput lost. It is for a source
  whose rules the broker would otherwise not see: an inbound-only channel (a client's write is not sent to
  the source), a model's types (OPC UA DataType, AAS `valueType`).
- **On the PATCH path,** only for the attributes a channel maps: one lookup for every other attribute,
  none when compiled out. Its own `COR_FEATURE_CHANNEL_CHECKS`, sized before it becomes a declared
  switch; an entry of the bridge ABI, NULL for a bridge without one. Not the Channel's owner rule: an
  owner decides *who* writes, the check *what* may be written.

## 3. An AssetChannel

The first bridge is OPC UA's (decided 2026-10-17):

```json
{ "opcua": { "server": { "url": "opc.tcp://press-17.acme.example:4840" },
             "ngsild": { "assetChannels": {
               "ns=2;s=Press17": {
                 "entities": [
                   { "from": "object", "typeDefinition": "ns=3;i=1002", "type": "Press" },
                   { "from": "object", "typeDefinition": "ns=3;i=1010", "type": "HydraulicUnit" } ],
                 "attributeLayout": "attributes" } } } } }
```

An AAS one, the same members:

```json
{ "aas": { "server": { "url": "http://aas-server:8081" },
           "ngsild": { "assetChannels": {
             "https://acme.example/ids/aas/press-17": {
               "entities": "natural",
               "attributeLayout": "subAttributes" } } } } }
```

- **The endpoint** (`channelTarget`) - the model's root: an OPC UA NodeId; an AAS shell's or submodel's id.
- **`entities`** - which nodes become entities. The data's owner decides, per channel; there is no default,
  and a channel without `entities` is refused:
  - `"natural"` (working name): the source's own units - each node of kind `object` (OPC UA), `shell` or
    `submodel` (AAS); its entity type the node's `typeName`, else its kind capitalized (`Shell`);
  - rules: by the node's kind (OPC UA `object`; AAS `shell`, `submodel`, `collection`) and optionally its
    meaning (OPC UA `typeDefinition`; AAS `semanticId`). Each rule names the entity type; the first rule a
    node matches wins. Nodes no rule names are not entities: their content goes into the entity above them
    (the root's, when the root is no entity, is dropped).
- **The entity id** - the node's own id when it is an IRI (AAS ids are: `https://acme.example/ids/aas/press-17`
  is a valid NGSI-LD id as it is); else `urn:ngsi-ld:<type>:<the names from the root to the node>`, each
  byte outside `[A-Za-z0-9._-]` as `%XX` (`urn:ngsi-ld:HydraulicUnit:Press17:Unit1`).
- **`attributeLayout`** - what a container that is not an entity becomes in the entity above it:
  - `attributes` (the default): each leaf an attribute, named by its path (`ContactInformation` / `Phone` ->
    `contactInformationPhone`; only `[A-Za-z0-9_]` kept, a space starting a word), each one queryable with `q`;
  - `subAttributes`: one Property named by the container, its value the container's name, its leaves
    sub-attributes (one level: a deeper container's leaves by their path);
  - `value`: the container one JsonProperty, its value as the source has it.

  The values are plain strings of the bridge's configuration (and of `GET /assetChannels`), never expanded
  as JSON-LD terms - `attributes` collides with nothing.
- **A node's meaning** (OPC UA: its HasDictionaryEntry references and its TypeDefinition; AAS: its
  semanticId) goes into a `semanticId` sub-Property of its attribute - always, no option. Not into the
  @context: there the meaning would hang on every reader's @context being complete, which no broker can
  check.
  - A node without a semanticId: its attribute has no `semanticId`.
  - A `semanticId` removed in the broker (an attribute replaced by a client; correct NGSI-LD, and with no
    owner, § 3.2, allowed): the channel does not depend on it - its rules match the source's semanticIds,
    never the broker's. The next write of its entity (the model again, or a value of a node in it) writes
    it again.

### 3.1 Elements to attributes (AAS; OPC UA alike)

| AAS element | NGSI-LD |
|---|---|
| Property (`valueType` xs:int, xs:double, xs:boolean, xs:string, xs:dateTime ...) | Property, the value converted |
| MultiLanguageProperty | LanguageProperty |
| Range | Property, value `{ "min": ..., "max": ... }` |
| File, Blob | Property: the file's URL (and `contentType` sub-Property); a Blob's content not copied |
| ReferenceElement to a node that is an entity | Relationship |
| ReferenceElement to anything else | Property, the reference as the source writes it |
| RelationshipElement (`first`, `second`) | a Relationship on `first`'s entity to `second`'s |
| a child that is an entity (the shell's submodels) | a Relationship named by the child (`nameplate`) |
| SubmodelElementList | ListProperty (of values) or ListRelationship (of references) |
| Operation | a **service** of the entity (§ 4) |
| BasicEventElement, Capability | not mapped in v1 |

OPC UA: a Variable -> Property (its DataType converted, `EngineeringUnits` -> `unitCode`, the
SourceTimestamp -> `observedAt`); HasComponent / HasProperty -> attributes; Organizes and the other
references between object nodes -> Relationships; a Method -> a service.

### 3.2 Behaviour

- **At start**, the Bridge reads the model (AAS: its HTTP/REST API - `GET /shells/{id}` and the submodels it
  references, ids base64url-encoded in the path; OPC UA: browse from the root) and writes the graph: one batch upsert, `options=update`, local - as a DataChannel's
  record is written: created when not there, the mapped attributes replaced, the others left alone.
- **A value that changes** (OPC UA: a monitored item; AAS: a server's events where it has them, else the
  model read again on `channelInfo`'s `pollInterval`) -> the bridge's `{ "values": [ ... ] }` (§ 3.3): the
  broker's kept copy of the model updated, and each entity a changed node is in written again, the same
  batch upsert - `observedAt` the node's `sourceTime`. A value of no node of the model is counted
  (`valuesRefused`) and dropped.
- **The model's structure that changes** (a submodel added, an element removed) -> the bridge hands the model
  over again: its entities and attributes are written; what the model no longer has is left alone (deleting
  it is not built).
- **Inbound only in v1**, as a DataChannel. Writing back (a PATCH -> the AAS `$value` PUT, an OPC UA Write) is
  a later step, then with a Channel's rule: transport first, a refusal stores nothing.
- **No owner**, as a DataChannel: an AssetChannel claims no attribute; the last write wins.

### 3.3 What a bridge hands over: the node tree

The seam is a Channel's - the bridge carries `(endpoint, json, time)` and knows nothing about entities
(decided 2026-10-17). On an AssetChannel's endpoint the json is a neutral node tree, the same from every
model bridge, so that every model maps the same way, in the broker:

```json
{ "model": {
    "node": "object", "id": "ns=2;s=Press17", "name": "Press17", "typeName": "Press",
    "typeDefinition": "ns=3;i=1002", "semanticIds": [ "0173-1#01-AKJ975#017" ],
    "children": [
      { "node": "folder", "name": "Hydraulics", "children": [
          { "node": "variable", "id": "ns=2;s=P17.Pressure", "name": "Pressure", "value": 212.5,
            "unit": "BAR", "sourceTime": "2026-10-17T10:00:00Z" } ] },
      { "node": "method", "name": "Start", "input": [], "output": [] } ],
    "references": [ { "type": "Organizes", "target": "ns=2;s=Line1" } ] } }
```

and, as values change:

```json
{ "values": [ { "id": "ns=2;s=P17.Pressure", "value": 214.0, "sourceTime": "2026-10-17T10:00:05Z" } ] }
```

| Member | |
|---|---|
| `node` | the node's kind, the source's word: `object`, `variable`, `folder`, `method` (OPC UA); `shell`, `submodel`, `collection`, `property`, `operation` (AAS) |
| `id`, `name` | the source's id (a NodeId, an AAS id) and name (BrowseName, idShort) |
| `typeName`, `typeDefinition` | its type's name, and id |
| `semanticIds` | its meaning: the `semanticId` sub-Property (§ 3) |
| `value`, `unit`, `sourceTime` | a leaf's value (JSON, converted by the bridge), `unitCode`, `observedAt` |
| `children` | the nodes it contains |
| `references` | `{ "type", "target" }`: a Relationship when the target is an entity node, named by the type (several of one type: a ListRelationship); a Property (the target's id) when it is not |

A `method` or `operation` makes nothing yet (§ 4). A sample that is neither `model` nor `values`, or a model
that makes no entity, is counted (`samplesRefused`) and dropped.

## 4. Operations and methods: Service Execution

**After the read side works** (decided 2026-10-17), one source at a time.

An AAS Operation and an OPC UA Method are what [Service Execution](service-execution.md) calls a service:
the asset does something when asked, with input and output arguments. An AssetChannel registers each one it
maps as a service of its entity (a ServiceRegistration, `serviceName` the Operation's idShort, its
`inputSchema` and `outputSchema` from the input and output variables), and the Bridge is its **executor** -
through the bridge executor contract (ABI 11) the IoT agents' commands use
([iot-agent-executor.md](iot-agent-executor.md)). `POST /entities/{id}/services/{name}` then invokes the
Operation; the execution's status follows the source's (AAS: the operation's invocation, sync or async;
OPC UA: the Call's result).

## 5. GET /ngsi-ld/v1/assetChannels

And `/{id}`, the id `urn:ngsi-ld:AssetChannel:<bridge>:<endpoint>`. The base (§ 2), and: the mapping as the
file wrote it (`assetEntities`), `attributeLayout`, `modelsIn`, `valuesIn`, `valuesRefused`,
`samplesRefused`, `entitiesWritten`, `payloadsRefused`. Without `COR_FEATURE_ASSET_CHANNELS`, an
`assetChannels` section is warned about and ignored, and `GET /assetChannels` answers 501.

## 6. Decisions

Decided (KZ, 2026-10-17):

1. **The name**: AssetChannel.
2. **What becomes an entity**: the data's owner decides, in the channel (`entities`, § 3) - no default.
3. **What a container becomes**: the member `attributeLayout`, its values `attributes` (the default),
   `subAttributes`, `value`.
4. **A node's meaning**: always a `semanticId` sub-Property - no @context mode, no option.
5. **Operations as services**: after the read side works, step by step.
6. **The first bridge**: OPC UA.
7. **How a bridge hands a model over**: on the existing seam, as a neutral node tree (§ 3.3) - no new bridge
   ABI, and the mapping in the broker, the same for every model bridge.

## 7. Plan

1. The shared base: Channel and DataChannel as it says (members, the Bridge listing them) - coraine, no new
   behaviour.
2. The payload check (§ 2.1), with the bridge ABI entry.
3. AssetChannel in the broker: the object, its listing, the write path (a DataChannel's batch upsert), the
   mapping vocabulary - `COR_FEATURE_ASSET_CHANNELS`, sized before it becomes a declared switch.
4. The OPC UA bridge, its own repository, on the bridge ABI as it is: the model and its values as the node
   tree (§ 3.3). The client library is to choose.
5. AAS, the second source.
6. Operations and methods as services (§ 4).
