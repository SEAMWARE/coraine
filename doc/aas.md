# Asset Administration Shells in coraine

What coraine is to do with an Asset Administration Shell (AAS): which parts of a shell become NGSI-LD,
in what shape, how the broker reaches the AAS server, and what is not decided yet. It is written for AAS
engineers; no source code is needed to follow it.

**Status: a prototype.** The broker's half - the AssetChannel, the mapping of a model into entities -
is built, and has two sources: the OPC UA bridge ([corOpcuaBridge](https://github.com/SEAMWARE/corOpcuaBridge))
and the AAS bridge, `aas.so` ([corAasBridge](https://github.com/SEAMWARE/corAasBridge), not released). The
examples below are what the mapping makes of an AAS; they are worked out by hand. The design in full is
[asset-channels.md](asset-channels.md); the bridge's configuration is in corAasBridge's README.

- [1. In one picture](#1-in-one-picture)
- [2. Why an AssetChannel](#2-why-an-assetchannel)
- [3. What you configure](#3-what-you-configure)
- [4. A shell, worked through](#4-a-shell-worked-through)
- [5. Element by element](#5-element-by-element)
- [6. How the bridge reaches the AAS server](#6-how-the-bridge-reaches-the-aas-server)
- [7. What an application sees](#7-what-an-application-sees)
- [8. Operations](#8-operations)
- [9. The mapping tool in c³](#9-the-mapping-tool-in-c)
- [10. Decided](#10-decided)
- [11. Open - for the AAS experts](#11-open---for-the-aas-experts)

## 1. In one picture

    AAS server  <-- AAS API (HTTP) -->  aas.so  <-->  coraine  <-- NGSI-LD -->  applications
    (shells, submodels)                 bridge         broker

The bridge (`aas.so`) is a plugin of the coraine broker and a **client** of one AAS server: it reads the
shells and submodels it is told to carry, and follows their values as they change. The broker turns what
it reads into NGSI-LD entities - stored, queried, subscribed to and kept in history like any entity a
client writes. The AAS server stays the owner of the model; coraine is a view of it, in NGSI-LD.

## 2. Why an AssetChannel

coraine connects outside sources to NGSI-LD through **channels**, and has three kinds, one per shape of
data:

| Shape | Example | Becomes | Channel kind |
|---|---|---|---|
| a signal - one value | a DDS topic, an OPC UA Variable, a Modbus register | one attribute | Channel |
| a record - a flat set of fields | an MQTT JSON message, a Kafka message | an entity | DataChannel |
| a **model** - a tree with types, meaning and references | **an AAS shell and its submodels**, an OPC UA Object | a **graph of entities** | **AssetChannel** |

An AAS is the model case: a shell refers to submodels, a submodel holds elements, elements hold
collections of elements, each element may carry a `semanticId` saying what it means, and references
point across the tree. An AssetChannel keeps all of that: the parts become entities, the elements
attributes, the references Relationships, the semanticIds stay with the attributes.

The same AssetChannel serves OPC UA. Both bridges hand the broker the same neutral tree, and the
broker maps both the same way - an AAS and an OPC UA model of one machine become NGSI-LD in one style.

## 3. What you configure

One entry per shell (or per submodel) to carry, in the broker's bridge configuration file:

```json
{ "aas": {
    "server": { "url": "http://aas-server:8081" },
    "ngsild": { "assetChannels": {
      "https://acme.example/ids/aas/press-17": {
        "entities": [
          { "from": "shell",    "type": "Press" },
          { "from": "submodel", "semanticId": "https://admin-shell.io/idta/nameplate/3/0/Nameplate", "type": "Nameplate" },
          { "from": "submodel", "type": "Submodel" } ],
        "attributeLayout": "attributes" } } } } }
```

- **The key** - the shell's id (or a submodel's): where the model starts.
- **`entities`** - which parts become entities. **You decide**, per channel; there is no default:
  - `"natural"`: every shell and every submodel an entity;
  - rules, as above: by the part's kind (`shell`, `submodel`, `collection`) and, optionally, its
    `semanticId`. Each rule names the entity type; the first rule a part matches wins. A part no rule
    names is not an entity - its content goes into the entity above it.
- **`attributeLayout`** - what a SubmodelElementCollection that is not an entity becomes inside the
  entity above it:
  - `attributes` (the default): each element in it an attribute of its own, named by its path -
    `AddressInformation` / `CityTown` -> `addressInformationCityTown`; each one queryable on its own;
  - `subAttributes`: one attribute for the collection, its elements as sub-attributes;
  - `value`: the collection as one JSON value, as the AAS server has it.

## 4. A shell, worked through

A press, `https://acme.example/ids/aas/press-17`, with two submodels - a Digital Nameplate (IDTA 02006,
version 3.0) and an operational-data submodel - abridged; the identifiers are the template's:

    Shell  Press17                              id https://acme.example/ids/aas/press-17
    ├─ Submodel  Nameplate                      id https://acme.example/ids/sm/press-17/nameplate
    │    semanticId https://admin-shell.io/idta/nameplate/3/0/Nameplate
    │    ├─ ManufacturerName   MultiLanguageProperty  en "ACME Presses", de "ACME Pressen"
    │    │                     semanticId 0112/2///61987#ABA565#009
    │    │                     supplementalSemanticId 0173-1#02-AAO677#004 (ECLASS)
    │    ├─ SerialNumber       Property xs:string  "P17-0042"
    │    │                     semanticId 0112/2///61987#ABA951#009
    │    │                     supplementalSemanticId 0173-1#02-AAM556#004 (ECLASS)
    │    └─ AddressInformation SubmodelElementCollection
    │         └─ CityTown      MultiLanguageProperty  de "Berlin"
    └─ Submodel  OperationalData                id https://acme.example/ids/sm/press-17/operational
         └─ Pressure           Property xs:double  212.5

With the configuration of section 3, the broker makes three entities.

**The shell** - an entity of type `Press`; its id is the shell's own (an AAS id is an IRI, and a valid
NGSI-LD entity id as it is). Each of its submodels is a Relationship, named by the submodel's idShort:

```json
{ "id": "https://acme.example/ids/aas/press-17", "type": "Press",
  "nameplate":       { "type": "Relationship", "object": "https://acme.example/ids/sm/press-17/nameplate" },
  "operationalData": { "type": "Relationship", "object": "https://acme.example/ids/sm/press-17/operational" } }
```

**The Nameplate** - type `Nameplate`, by its semanticId's rule. Each element an attribute; each
semanticId a `semanticId` sub-Property of its attribute (the element's `semanticId`; what to do with its
`supplementalSemanticIds` is open, section 11); the collection flattened (`attributes`) - the address's
own semanticIds left out here for brevity:

```json
{ "id": "https://acme.example/ids/sm/press-17/nameplate", "type": "Nameplate",
  "manufacturerName": { "type": "LanguageProperty",
                        "languageMap": { "en": "ACME Presses", "de": "ACME Pressen" },
                        "semanticId": { "type": "Property", "value": "0112/2///61987#ABA565#009" } },
  "serialNumber":     { "type": "Property", "value": "P17-0042",
                        "semanticId": { "type": "Property", "value": "0112/2///61987#ABA951#009" } },
  "addressInformationCityTown": { "type": "LanguageProperty", "languageMap": { "de": "Berlin" } } }
```

The same Nameplate with `"attributeLayout": "subAttributes"` keeps the collection as one attribute:

```json
  "addressInformation": { "type": "Property", "value": "AddressInformation",
                          "cityTown": { "type": "LanguageProperty", "languageMap": { "de": "Berlin" } } }
```

**OperationalData** - no rule matches its semanticId, so the last rule makes it a `Submodel`:

```json
{ "id": "https://acme.example/ids/sm/press-17/operational", "type": "Submodel",
  "pressure": { "type": "Property", "value": 212.5, "observedAt": "2026-10-17T10:00:00Z" } }
```

When the pressure changes at the source, the `pressure` attribute is written again, with the time the
value had at the source as its `observedAt`.

## 5. Element by element

| AAS element | NGSI-LD |
|---|---|
| Property (`xs:int`, `xs:double`, `xs:boolean`, `xs:string`, `xs:dateTime` ...) | Property, the value converted to JSON (a number, a boolean, a string) |
| MultiLanguageProperty | LanguageProperty |
| Range | Property, value `{ "min": ..., "max": ... }` |
| File | Property: the file's URL (a file of the server's AASX package: the API's attachment URL), and a `contentType` sub-Property |
| Blob | Property: the API's attachment URL, and a `contentType` sub-Property; the content itself is not copied |
| ReferenceElement to a part that is an entity | Relationship |
| ReferenceElement to anything else | Property, the reference as the AAS writes it |
| RelationshipElement (`first`, `second`) | a Relationship on `first`'s entity, to `second`'s - not mapped by the prototype |
| a submodel of a shell, a part that is an entity | a Relationship named by its idShort |
| SubmodelElementList | ListProperty (of values) or ListRelationship (of references) |
| SubmodelElementCollection | an entity (when a rule says so), else by `attributeLayout` (section 3) |
| Operation | a service, executed by the bridge (section 8) |
| BasicEventElement, Capability, Entity, AnnotatedRelationshipElement | not mapped |

**The leaf's kind.** The node tree a bridge hands over says it: a leaf's `attributeType` -
`LanguageProperty` (its value a language map) or `ListProperty` (an array) - and its `contentType`
([asset-channels.md § 3.3](asset-channels.md#33-what-a-bridge-hands-over-the-node-tree)).

**Names.** An attribute's name is the element's idShort with a lower-case first letter; a path of
idShorts (`attributes` layout) joins them, each one after the first starting upper-case. Only
`[A-Za-z0-9_]` is kept.

**Meaning.** An element's `semanticId` is always written as a `semanticId` sub-Property of its
attribute - not left to an @context. The prototype takes the reference's first key. An application finds what an attribute means in the entity itself,
whatever @context it uses; an element without a semanticId has no `semanticId` sub-Property.

**Ids.** A shell's or submodel's id is used as the entity id unchanged. A collection that a rule makes an
entity has no id of its own in AAS; its id is made from the path to it:
`urn:ngsi-ld:<type>:<idShort>:<idShort>...`.

## 6. How the bridge reaches the AAS server

Through the AAS HTTP API (IDTA Part 2), as any AAS client does:

- **At start:** `GET /shells/{id}`, then each submodel it refers to, `GET /submodels/{id}` - ids
  base64url-encoded in the path, as the API says. The whole model is written in one batch: the entities
  created when not there, their mapped attributes replaced, any other attribute on them left alone.
- **Values that change:** the model read again every `pollInterval` (set per channel, `channelInfo`, an
  ISO 8601 duration; 10 s by default). Each entity a changed element is in is written again, the time of
  the read its `observedAt`. The server's events are not used.
- **The model's structure changes** (a submodel added, an element removed): the model is written again.
  What the model no longer has is **left alone**, not deleted - a server restarting or a connection lost
  must not wipe the last values, and a reconnect may bring it back.
- **Writing back,** with the channel's `channelDirection` `both`: a client's write to one of these
  attributes goes to the AAS server first, `PATCH /submodels/{id}/submodel-elements/{idShortPath}/$value`
  in the ValueOnly form (Property, MultiLanguageProperty, Range, a SubmodelElementList of Properties). A
  File, a Blob and the asset information are not written: 422. A write the server refuses is read back -
  the entity shows what the server has. Inbound only (the default), a client's write is stored in the
  broker and not sent.
- **The payload check.** The bridge may refuse a client's write that does not fit the element's
  `valueType` (a string to an `xs:double`): the client gets 400 and nothing is stored.
- **When the server cannot be reached,** the bridge retries, and the broker says why nothing comes in -
  in `GET /ngsi-ld/v1/assetChannels`, its `status` and `statusReason`.

## 7. What an application sees

Plain NGSI-LD; nothing AAS-specific is needed to read it:

- every nameplate: `GET /ngsi-ld/v1/entities?type=Nameplate`
- a press and its submodels in one answer: `GET /ngsi-ld/v1/entities/{press}?join=inline`
- by meaning: `q=serialNumber.semanticId=="0112/2///61987#ABA951#009"`
- a value's history, when the broker keeps history: the temporal API on `pressure`
- a subscription on `pressure`, notified as the source changes

One limit: `q` names an attribute, so "every attribute whose semanticId is X, whatever its name" is not
a single query.

## 8. Operations

An AAS Operation is what NGSI-LD Service Execution calls a service: the asset does something when asked,
with input and output arguments. A Service Registration whose endpoint is the Operation -
`aas+http[s]://<server>/submodels/<id, base64url>/submodel-elements/<idShortPath>` - makes it a service the
AAS bridge executes ([service-execution.md](service-execution.md)): the execution's input becomes the
Operation's input arguments by idShort, `POST .../invoke` is called, and the output arguments by idShort
are the execution's output. A failed invocation fails the execution (502). Only Property variables are
taken.

The registrations are made by hand. Registering each Operation of a model as a service of its entity,
when the model is read ([asset-channels.md § 4](asset-channels.md#4-operations-and-methods-service-execution)), is not built.

## 9. The mapping tool in c³

c³, coraine's control center, gets a mapping page: it browses what a bridge's source offers - an OPC UA
server's address space first, an AAS server's shells and submodels next - lets you pick what to carry,
and writes the configuration of section 3. The broker's part is an
endpoint that asks a bridge for its source's inventory (`GET /admin/bridges/{bridge}/inventory`); the AAS
bridge answers it with the server's shells and submodels, and with `?from=<id>` that shell's model.

## 10. Decided

1. An AAS is carried by an **AssetChannel** - the same channel kind, mapping and configuration as an
   OPC UA object model.
2. **What becomes an entity** is the data's owner's choice, per channel - `natural` or rules by kind and
   semanticId. There is no default.
3. **A collection** that is not an entity: `attributeLayout` `attributes` (the default), `subAttributes`
   or `value`.
4. **A semanticId** is always a `semanticId` sub-Property, never an @context matter.
5. **Ids:** an AAS id is the entity id unchanged.
6. **What the model no longer has** is left alone.
7. **Operations** become services, executed by the bridge.
8. **The order:** OPC UA first, AAS second - both built.

## 11. Open - for the AAS experts

Not decided; the answers decide the AAS bridge:

1. **Which servers first.** Eclipse BaSyx, the Eclipse AASX Server, FA³ST Service - which ones the users
   run, and which API version (V3.0, V3.1) to target first.
2. **Live values.** Which servers can push changes, and how (MQTT, server-sent events) - or is polling
   the normal case? Many AAS servers take their live values from OPC UA themselves: should coraine take
   the structure and the meaning from the AAS and the live values straight from OPC UA?
3. **Registries.** Should the bridge find shells through an AAS Registry (`/shell-descriptors`), or
   always be given the server and the shell ids?
4. **The shell's entity type.** With `natural`, a shell is of type `Shell`. Is there something better to
   take by default - `assetInformation`'s `assetType`, the asset kind?
5. **The AAS `Entity` element** (a part in a bill of material, with its own `globalAssetId`) and
   **AnnotatedRelationshipElement** are not mapped yet (section 5). An `Entity` element looks
   like an NGSI-LD entity of its own - is that how AAS users see it?
6. **semanticId detail.** A semanticId is a reference of one or more keys, and an element may have
   `supplementalSemanticIds`. The Digital Nameplate 3.0 gives each element an IEC CDD id as its semanticId
   and the ECLASS id as a supplemental one (SerialNumber: `0112/2///61987#ABA951#009`, and
   `0173-1#02-AAM556#004`). Is the semanticId's first key enough, or must the supplemental ids be kept
   too - the ECLASS id is the one many users search by?
7. **ConceptDescriptions.** Should their content (preferred names, units, definitions) be carried too -
   as entities of their own, or as a `unitCode` and sub-Properties on the attributes?
8. **AASX packages.** Is importing an AASX file, with no server, worth a path of its own?
9. **Security.** How AAS servers are protected in practice (OAuth2 tokens, mutual TLS, an API key), so
   the bridge can authenticate.
10. **Writing back.** Which elements users expect to write through NGSI-LD, if any - or is read-only
    what an AAS view should be?
