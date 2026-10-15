# ModelChannels - a source model as a graph of entities (design, for review)

**Status: a design for review - nothing of it is built.** The name ModelChannel is a working name.

A [Channel](bridge-channels.md) ties one endpoint to one attribute; a [DataChannel](bridge-channels.md#datachannels)
makes each record of an endpoint one or more entities. Neither fits a source whose data is a **model**: an
Asset Administration Shell with its submodels, an OPC UA server's objects. There one asset is a tree of
elements, typed and with meaning attached (a semanticId, a type definition), with references across it -
and it becomes a **graph of entities**, with Relationships where the model has references. A ModelChannel
is that mapping.

## 1. Three shapes of data, three channel types

| | One message, one reading | Becomes | Type |
|---|---|---|---|
| **Signal** | one value: a DDS topic sample, an OPC UA variable, a Modbus register | one attribute of one entity | Channel |
| **Record** | a flat object of fields: MQTT JSON (an IoT agent's measures), a Kafka message, a CKAN row | an entity or a few, a field per attribute | DataChannel |
| **Model** | a tree with types, meaning and references: an AAS shell and its submodels, an OPC UA object and its components | a graph of entities and Relationships | **ModelChannel** |

Three types, not one with a sub-type: their members differ (an attribute and an entity; a column mapping
and an id template; a model root and node rules), and a type whose members depend on a sub-type is what
OpenAPI describes worst - and what every client must branch on (decided 2026-10-16).

## 2. What the three share

Every channel type is the same base, and adds its own members:

| Member | |
|---|---|
| `id`, `type` | `urn:ngsi-ld:<Type>:<bridge>:<endpoint>`; `Channel`, `DataChannel`, `ModelChannel` |
| `bridgeId` | the Bridge that carries it - every channel points to its Bridge |
| `endpoint` | what the Bridge reads: a topic, a register, a resource id, a model root |
| `channelInfo` | the endpoint's settings, the bridge's own (key, value) |
| `status`, `statusReason` | `available`, `dormant`, and why (`channelStatusIn`) |
| counters | what came in, what was refused - each type its own |

In OpenAPI: one base schema, each type `allOf` it. The Bridge lists what it carries - its Channels,
DataChannels and ModelChannels - so "everything this bridge does" is one request without one polymorphic
collection. (Today Channel and DataChannel share these members by convention; the base makes it a rule.)

## 3. A ModelChannel

```json
{ "aas": { "server": { "url": "http://aas-server:8081" },
           "ngsild": { "modelChannels": {
             "https://acme.example/ids/aas/press-17": {
               "entities": [
                 { "from": "shell",                                   "type": "Asset" },
                 { "from": "submodel",  "semanticId": "https://admin-shell.io/zvei/nameplate/2/0/Nameplate",
                                                                      "type": "Nameplate" },
                 { "from": "submodel",  "semanticId": "https://acme.example/semantics/OperationalData",
                                                                      "type": "OperationalData" } ],
               "nested": "attributes",
               "semanticIds": "subProperty" } } } } }
```

- **`endpoint`** - the model's root: an AAS shell's or submodel's id; an OPC UA NodeId.
- **`entities`** - which nodes become entities: by their kind (`shell`, `submodel`, `collection`, an OPC UA
  `object`) and optionally their meaning (`semanticId`; for OPC UA a `typeDefinition`). Each rule names the
  entity type. Nodes no rule names are not entities: their content goes into the entity above them.
- **The entity id** - the node's own id when it is an IRI (AAS ids are: `https://acme.example/ids/aas/press-17`
  is a valid NGSI-LD id as it is); else a template on the node's path (`urn:ngsi-ld:{type}:{shellIdShort}:{path}`).
- **`nested`** - what a container that is not an entity becomes in the entity above it:
  - `attributes`: each leaf an attribute, named by its path (`ContactInformation.Phone` -> `contactInformationPhone`);
  - `json`: the container one JsonProperty, as the source has it;
  - `subAttributes`: one attribute, its leaves sub-attributes (one level only).
- **`semanticIds`** - where a node's meaning goes: `subProperty` (each attribute a `semanticId`
  sub-Property), `context` (the @context maps the attribute's name to the semanticId's IRI - the meaning
  becomes the name), or `none`.

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
| the shell's submodel references | Relationships `hasSubmodel` (shell -> submodel entities) |
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
  model read again on `channelInfo`'s `pollInterval`) -> the attribute's PATCH, `observedAt` the source's time
  where it gives one.
- **The model's structure that changes** (a submodel added, an element removed) -> its entities and
  attributes created or deleted - or left alone, with `structure: ignore`.
- **Inbound only in v1**, as a DataChannel. Writing back (a PATCH -> the AAS `$value` PUT, an OPC UA Write) is
  a later step, then with a Channel's rule: transport first, a refusal stores nothing.
- **No owner**, as a DataChannel: a ModelChannel claims no attribute; the last write wins.

## 4. Operations and methods: Service Execution

An AAS Operation and an OPC UA Method are what [Service Execution](service-execution.md) calls a service:
the asset does something when asked, with input and output arguments. A ModelChannel registers each one it
maps as a service of its entity (a ServiceRegistration, `serviceName` the Operation's idShort, its
`inputSchema` and `outputSchema` from the input and output variables), and the Bridge is its **executor** -
through the bridge executor contract (ABI 11) the IoT agents' commands use
([iot-agent-executor.md](iot-agent-executor.md)). `POST /entities/{id}/services/{name}` then invokes the
Operation; the execution's status follows the source's (AAS: the operation's invocation, sync or async;
OPC UA: the Call's result).

## 5. GET /ngsi-ld/v1/modelChannels

The base (§ 2), and: `modelRoot` (the endpoint), the mapping as the file wrote it, `entitiesWritten`,
`attributesWritten`, `valuesIn`, `nodesRefused`, `servicesRegistered`.

## 6. To decide

1. **The name** - ModelChannel, GraphChannel, AssetChannel?
2. **What becomes an entity by default**: the shell and each submodel (the natural unit of AAS), or the
   rules only?
3. **`nested`'s default**: `attributes` (queryable with `q`), or `json` (the source's shape)?
4. **`semanticIds`'s default**: a sub-Property, or the @context?
5. **Operations as services in v1**, or after the read side works?
6. **The first bridge**: AAS over its REST API (an AAS server - Eclipse BaSyx, FA³ST - in a container for the
   tests), or OPC UA (a library to choose)?

## 7. Plan

1. The shared base: Channel and DataChannel as it says (members, the Bridge listing them) - coraine, no new
   behaviour.
2. ModelChannel in the broker: the object, its listing, the write path (a DataChannel's batch upsert), the
   mapping vocabulary - `COR_FEATURE_MODEL_CHANNELS`, sized before it becomes a declared switch.
3. The first bridge (§ 6.6), its own repository, the bridge ABI as today plus what a model needs (a model
   read and its changes, a graph of nodes).
4. Operations as services (§ 4).
