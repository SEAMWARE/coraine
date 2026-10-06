# coraine Roadmap

This product is an Incubated FIWARE Generic Enabler. If you would like to learn
about the overall Roadmap of FIWARE, please check the section "Roadmap" on the
[FIWARE Catalogue](https://www.fiware.org/developers/catalogue/).

## Introduction

This section elaborates on proposed new features or tasks which are expected to be
added to the product in the foreseeable future. There should be no assumption of a
commitment to deliver these features on specific dates or in the order given. The
development team will be doing their best to follow the proposed dates and
priorities, but please bear in mind that plans to work on a given feature or task
may be revised. All information is provided as general guidelines only, and this
section may be revised to provide newer information at any time.

The detailed, day-to-day backlog — including what is deferred *by design* and why —
is [`ToDo.md`](https://github.com/SEAMWARE/coraine/blob/main/ToDo.md) in the root of this repository. It is kept current as
work lands rather than at release boundaries. Every idea, with what there is to say about it, is in
[Ideas](ideas.md); the lists below are one line each, linked to it.

## Available now

Recently built, and documented where it lives:

-   **DDS** - topics both ways, services, actions, through the DDS bridge ([DDS and ROS 2](dds.md)).
-   **MQTT** - device topics as Channels, and notifications to `mqtt://` ([The MQTT bridge](mqtt-bridge.md)).
-   **Modbus TCP** - registers as Channels ([The Modbus bridge](modbus-bridge.md)).
-   **WebSocket** - the whole API over one connection, and notifications on it ([WebSocket](websocket.md)).
-   **`cor://`** - the binary protocol for GE-to-GE traffic: forwarding without a JSON parse, many
    requests in flight on one connection ([The cor format and cor://](cor-protocol.md)).
-   **Requests that wait run as coroutines** of the event loops, not on worker threads
    ([Coroutines](coroutines.md)).
-   **An NGSI-LD-aware JSON parser** - the JSON parser unchanged, the core terms stamped as ids by a
    hook during the parse ([Ideas](ideas.md#an-ngsi-ld-aware-json-parser)).
-   **corDB on disk** - `--dbDir`: an append log synced every 100 ms and snapshots; an attribute update
    logs the attributes, not the entity; `--dbCompress`; the store survives a restart, still with no
    database server ([Installation](installation.md), "corDB on disk").
-   **One system timestamp per entity in corDB** - an entity is created with one time; below it only
    the times that differ are stored: 21-30 % less memory, faster writes ([corDB's README](https://github.com/SEAMWARE/corDB#readme)).
-   **History in corDB** - `--troe corDB`: the temporal API from the store's own process, no second
    database ([Installation](installation.md)).
-   **Service Execution** - "do this" in the API, not a write to an attribute: service registrations,
    executions with their lifecycle and notifications, combined and grouped executions, after the ETSI
    report GR CIM-055 - under conditional compilation. Its documentation follows the report's
    publication.

Bridges and Channels are coraine's own mechanism, not a standard: the concept goes to the ETSI TC DATA
face-to-face in Athens, 20–22 October 2026, and anything normative will realistically follow in 2027.
coraine implements its own objects now and adapts to whatever TC DATA settles on.

## Short term

The following features are planned to be addressed in the short term and
incorporated in the next release of the product, in roughly this order:

-   **One system timestamp per entity in MongoDB** - as corDB has it: smaller documents, a smaller cache. ([more](ideas.md#one-system-timestamp-per-entity---in-mongodb-too))
-   **The whole NGSI-LD API on corDB** - Snapshots first. ([more](ideas.md#the-whole-ngsi-ld-api-on-cordb---snapshots-first))
-   **A memory budget and admission control** - refuse writes before the OOM killer. ([more](ideas.md#a-memory-budget-and-admission-control))
-   **An ARM image, and a more diverse nightly** - ARM64; sanitizers, clang, musl. ([more](ideas.md#an-arm-image-and-a-more-diverse-nightly))
-   **Finish conditional compilation** - every feature flag reaches the code it names. ([more](ideas.md#finish-conditional-compilation))
-   **Authorisation** - inside the broker, and as an APISIX plugin; ODRL, verifiable credentials. ([more](ideas.md#authorisation-inside-the-broker-and-as-an-apisix-plugin))
-   **Migrating from Orion-LD** - a converter for its database; later, the ETSI neutral export format. ([more](ideas.md#migrating-from-orion-ld))
-   **Service Execution through the DDS and Modbus bridges** - ROS 2 services and actions, Modbus writes, as services. ([more](ideas.md#service-execution))
-   **More bridges** - Kafka, WebSockets, OPC UA on the Bridge/Channel seam. ([more](ideas.md#more-bridges))
-   **Packages** - `apt-get install coraine`. ([more](ideas.md#packages))
-   **Subordinate subscriptions on registration change** - § 10.5.2.4 for `PATCH` too. ([more](ideas.md#subordinate-subscriptions-on-registration-change))

## Medium term

The following specific features are proposed to be addressed in the medium term,
typically within the subsequent release(s) generated in the next **9 months**:

-   **What history records in corDB** - a selector, "the last X", the temporal index. ([more](ideas.md#automatic-troe-in-cordb))
-   **The IoT Agents as cor-agent plugins** - device protocols on the bridge contract; one binary or two tiers. ([more](ideas.md#the-iot-agents-as-cor-agent-plugins))
-   **A Grafana data source** - dashboards over the current state and the temporal API. ([more](ideas.md#a-grafana-data-source))
-   **Aligning Bridges and Channels with ETSI** - 2027, after the Athens face-to-face. ([more](ideas.md#aligning-bridges-and-channels-with-etsi))
-   **Bridges and Channels over the API** - create, update, delete, persisted. ([more](ideas.md#bridges-and-channels-over-the-api))
-   **haaux** - high-availability cache sync without a shared database. ([more](ideas.md#haaux))

## Long term

The following are proposals regarding the longer-term evolution of the product.
Take into account that there is no commitment to deliver them in a specific
timeframe; they are provided so that potential contributors can see where the
product is heading and may wish to get involved.

-   **Our own string collation, replacing ICU** - root collation without 39 MiB of libicu. ([more](ideas.md#our-own-string-collation-replacing-icu))
-   **Array reduction in corJsonld** - done today in five places; once, term-aware, at the input boundary. ([more](ideas.md#array-reduction-in-corjsonld))
-   **Embedded deployment** - constrained hardware as a build configuration. ([more](ideas.md#embedded-deployment))
-   **Continuous ETSI conformance** - 100 % kept as the specification evolves. ([more](ideas.md#continuous-etsi-conformance))
-   **Broader performance regression coverage** - more scenarios, nightly. ([more](ideas.md#broader-performance-regression-coverage))

Everything else - open questions, smaller improvements, proposals to ETSI - is in [Ideas](ideas.md).
