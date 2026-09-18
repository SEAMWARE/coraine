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
work lands rather than at release boundaries.

## Short term

The following features are planned to be addressed in the short term and
incorporated in the next release of the product, in roughly this order:

-   **DDS.** Speak DDS natively, so a robotics or industrial deployment can put
    a context broker where it previously needed a gateway. DDS topics become
    entity attributes in both directions: a sample published on `rt/pose` updates
    `(urn:ngsi-ld:robot:1, pose)`, and a change to that attribute publishes a
    sample.

    This is first on the list, and it does not wait for anybody. DDS is the
    first peer that is *not* an NGSI-LD broker and *not* HTTP, so everything the
    broker assumes about request/response has to be made explicit to
    accommodate it — and that mechanism is ours to design and ship now. A
    **Bridge** is the transport instance (for DDS the participant — domain, QoS
    defaults, types directory); a **Channel** ties one foreign endpoint to one
    entity attribute with a direction and a retention. The design notes behind
    that (`doc/bridge-channels.md`) are not published while the concept is in
    front of ETSI.

    **Standardisation is a separate, slower track.** The same concept is being
    taken to ETSI — presented at the TC DATA face-to-face in Athens, 20–22
    October 2026 — and anything that becomes normative there will realistically
    land in 2027. Waiting for it would mean no DDS for a year and a half. So
    coraine implements its own Bridge and Channel objects now and adapts to
    whatever TC DATA settles on, which is the cheaper direction to be wrong in:
    an implementation that exists can be aligned, a specification nobody
    implemented cannot be validated.

-   **`cor://` — a binary protocol for GE-to-GE traffic.** TLV-framed, beside
    REST rather than instead of it, with no JSON parse on the hot path. It covers
    broker-to-broker forwarding, which is where NGSI-LD currently pays for HTTP
    and JSON on every hop of a federation.

    Its serialisation is deliberately the same algorithm that persistence will
    use, so the format is designed once for both. Doing it the other way round
    produces two formats that diverge, and the second one arrives as "the other
    serializer".

-   **corDB: persistence, and temporal history for free.** corDB is already the
    current-state store — entities in the process's own RAM, no database server,
    and [measurably faster](performance.md) than going through MongoDB. Two
    things finish it:

    **Persistence.** Today a corDB deployment does not survive a restart, which
    is the one place the "no servers" configuration is weaker than the MongoDB
    one. Snapshot plus a write-ahead log, with group-commit `fsync` on a timer —
    the durability MongoDB gives by default — keeps the write path as fast as it
    is now.

    **Automatic TRoE.** Temporal history in the same process and the same tree,
    reached by a boolean rather than by loading a second plugin that keeps its
    own copy of the store. Measured on eight shared cores, in-process history
    costs nothing; the same history in PostgreSQL costs corDB 91% of its write
    rate.

-   **Bridges and Channels, generalised.** Once DDS has forced the seam into
    existence, the same contract carries the rest: `cor://`, MQTT, OPC UA,
    WebSockets. The protocol names the endpoint and the endpoint decides the
    transport — a subscription or a registration asks for one by the scheme of
    its endpoint, and HTTP stays inline rather than becoming a plugin, because
    HTTP is also the NGSI-LD REST API and the broker can never ship without it.

    This is our mechanism, not a standard. See the note under DDS above.

-   **Service Execution.** Actuation as a first-class citizen of the API, beyond
    the suggested workflows of TS 104 175 Annex G. DDS makes this concrete rather
    than theoretical: DDS services and actions are in scope, so the broker needs
    a way to express "do this" that is not a write to an attribute.

-   **Packages, so nobody has to build it.** A Debian repository and
    `apt-get install coraine`, with a `coraine-dev` that pulls the whole
    dependency stack in one command. Building from source is currently the only
    route to a machine that is not running the container image, and
    [Building from source](building.md) is a long page for what should be one
    line.

-   **Finish conditional compilation.** Per-feature `#ifdef`s so a deployment
    compiles only the NGSI-LD it uses. The mechanism exists and the first slice
    landed — `REGISTRATIONS` and `SUBSCRIPTIONS` compile out, and the HTTP server
    is already a build choice — but most of the declared feature flags do not yet
    reach the code they name. [Building from source](building.md) says exactly
    which, because a flag that reports as off while the feature still works is
    worse than no flag.

-   **Subordinate subscriptions on registration change.** § 10.5.2.4 currently
    handles creation and deletion but not `PATCH`.

## Medium term

The following specific features are proposed to be addressed in the medium term,
typically within the subsequent release(s) generated in the next **9 months**:

-   **The IoT Agents, as cor-agent plugins.** Parity with the existing FIWARE
    IoT Agents — UltraLight, JSON, LWM2M, LoRaWAN, Sigfox, OPC UA, ISOXML — but
    as plugins on the bridge contract rather than as separate processes with
    separate deployments. The same source code compiles to a reduced **cor-agent**
    configuration, so the two-tier split of agent-then-broker becomes a deployment
    choice: a small edge build beside a central broker, or one binary doing both,
    which is what a FIWARE@Home installation on a Raspberry Pi actually wants.

    See [Speaking to devices directly](device-protocols.md) for the deployment
    shapes and the transport × payload split that keeps the plugin count down.

-   **CorSec — a security Generic Enabler.** Authorisation in front of the
    broker, as a plugin for [APISIX](https://apisix.apache.org/), so that a
    FIWARE deployment gets policy enforcement without the broker pretending to
    be an identity manager. NGSI-LD defines no authentication or authorisation,
    and coraine implements the specification; this is the piece that belongs
    beside it rather than inside it.

-   **More cor-agent plugins**, driven by what deployments actually ask for
    rather than by completing a matrix.

-   **Aligning Bridges and Channels with ETSI.** Expected in **2027**, on
    whatever TC DATA standardises after the Athens face-to-face of October 2026.
    Budgeted as adaptation work rather than as new capability: the objects, the
    endpoint-scheme convention and the codec seam are likely to move, and the
    transports carried over them are not. Listed here so that nobody reads
    coraine's Bridge and Channel as already-standard NGSI-LD — they are not, and
    the design notes say so on their first page.

-   **OPC UA.** Variables as attributes, monitored items as subscriptions,
    methods as Service Execution.

-   **WebSockets.** Notification delivery to consumers that cannot themselves be
    HTTP servers — anything behind NAT, a firewall or a browser.

-   **haaux.** High-availability cache synchronisation without a shared database:
    brokers register with each other at startup, keep the connection, and sync
    subscriptions, registrations and contexts interrupt-driven in single-digit
    milliseconds. No polling.

## Long term

The following are proposals regarding the longer-term evolution of the product.
Take into account that there is no commitment to deliver them in a specific
timeframe; they are provided so that potential contributors can see where the
product is heading and may wish to get involved.

-   **Authorisation inside the broker.** If CorSec in front of the broker proves
    to be the bottleneck rather than the policy, attribute-based access control
    evaluated in the broker is the most performant place to put it — the broker
    already holds the entity and the request, so it is the only component that
    can decide without a second round trip. This is listed as a possibility, not
    a plan: putting policy inside a specification-complete broker is a decision
    to take slowly.

-   **An NGSI-LD-aware JSON parser.** The core terms are a closed set, so
    `type`, `value`, `observedAt`, `Property`, `Relationship` and the rest can be
    an enum rather than a string — smaller on the wire and on disk, and a compare
    rather than a `strcmp` everywhere in the broker. Done once, gained always,
    and it is the same decision as the `cor://` format above.

-   **Our own string collation, replacing ICU.** § 7.6.2.1 makes ICU "root"
    collation the default order for `orderBy` on strings, and honouring it with
    libicu costs three shared libraries and 39.2 MiB — `libicudata` alone is
    31.6 MiB, nine times the size of the broker, for a table. That is the single
    largest thing coraine can put on a machine and it is there for one sort
    order.

    So: an MVP of root collation in `corNgsild` — UTF-8 decoded to code points,
    a primary/secondary/tertiary weight table covering the Latin ranges that
    real deployments use, and the category order (punctuation < digits <
    letters) that the current ASCII approximation gets wrong. Locale tailorings
    (`collation=sv`, `collation=de-DE-u-co-phonebk`, …) added **on demand**, one
    at a time, rather than by linking every locale on Earth in advance. ICU
    stays available behind `COR_FEATURE_ICU_COLLATION=ON` for anyone who wants
    the complete Unicode answer.

    `test/funcTests/cases/orderby_collation_locale.test` is the discriminator
    that already exists: it is written to fail on a build that does not
    implement § 7.6.2.1, and it is what the MVP has to turn green without ICU.

-   **Array reduction in `corJsonld`.** A single JSON-LD normalisation applied
    once at the input boundary rather than at each call site.

-   **Embedded deployment.** The broker adds 4.3 MiB to a machine, holds 17 MiB
    resident and answers 12 ms after `exec`; running it on constrained hardware
    is a question of build configuration, not redesign — the per-feature
    `#ifdef`s above are what make that true rather than aspirational.

-   **Continuous ETSI conformance.** Keeping the official test suite at 100% as
    the specification evolves, and feeding test-side corrections upstream.

-   **Broader performance regression coverage.** Measured nightly and recorded,
    so a regression is noticed by CI rather than by a user.
