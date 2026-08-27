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
incorporated in the next release of the product:

-   **Service Execution.** Actuation as a first-class citizen of the API, beyond
    the suggested workflows of TS 104 175 Annex G.

-   **The endpoint decides the transport.** HTTP is built in and always present -
    it is also the NGSI-LD REST API, so the broker can never ship without it. What
    the *bridge* seam adds is the ability to use a different transport INSTEAD, when
    a subscription or a registration asks for one by the scheme of its endpoint. The
    protocol names the endpoint; `bridge` names the seam.

-   **Binary IPC protocol.** A TLV-framed transport beside REST, with no JSON
    parse on the hot path.

-   **Finish conditional compilation.** Per-feature `#ifdef`s, so a deployment
    compiles only the NGSI-LD it uses.

-   **A choice of HTTP implementation.** `corRest` is built on libmicrohttpd,
    which supplies a great deal for nothing: TLS, a thread pool, epoll, the
    connection lifecycle, header parsing and the HTTP/1.1 upgrade machinery. It
    is also 607 KiB of shared library against a 967 KiB stripped broker — a
    price that matters exactly where the broker is most interesting, on a device
    with little storage and less RAM.

    So: conditional compilation, and `corRest` builds against either
    libmicrohttpd or a lean in-house HTTP implementation — or, in principle, a
    third library. An experimental implementation of the second already exists
    and is tested. Only four of `corRest`'s thirty-six sources touch
    libmicrohttpd today, so the seam is already close to where it needs to be.

    **Decided by measurement, not preference.** Both builds get compared on
    executable size and on throughput and latency under the same load, and the
    numbers are published rather than asserted. Neither becomes the default
    until that exists.

    The trade to go in with eyes open: libmicrohttpd ships a WebSocket helper
    alongside the upgrade support, so the lean build gives that up and has to
    implement the upgrade handshake and framing itself. WebSockets are on the
    medium-term list below, which makes these two decisions one decision.

-   **Subordinate subscriptions on registration change.** § 10.5.2.4 currently
    handles creation and deletion but not `PATCH`.

## Medium term

The following specific features are proposed to be addressed in the medium term,
typically within the subsequent release(s) generated in the next **9 months**:

-   **DDS transport.** Speak DDS natively for robotics and industrial deployments,
    addressed as `dds://` endpoints.

-   **OPC UA transport.** The same for industrial automation: variables as
    attributes, monitored items as subscriptions, methods as Service Execution.

-   **WebSockets.** Notification delivery to consumers that cannot themselves be
    HTTP servers — anything behind NAT, a firewall or a browser. How much of this
    comes for free depends on the HTTP implementation chosen above: on
    libmicrohttpd the upgrade handshake and framing are largely provided, and on
    the lean build they are work.

-   **corDB.** An NGSI-LD-aware store with entities cached in RAM and persistence
    behind it, replacing translation with representation.

-   **haaux.** High-availability cache synchronisation without a shared database,
    single-digit milliseconds, interrupt driven.

-   **Speaking to devices directly.** A **south bridge**: the broker itself able
    to talk to devices — MQTT, CoAP/LWM2M, OPC-UA, LoRaWAN, Sigfox, UltraLight,
    JSON, ISOXML, CSV, Kafka — as plugins on the same contract the DDS bridge
    introduces. The two-tier split of agent-then-broker becomes a deployment
    choice rather than a requirement: a small edge build (**cor-agent**) beside a
    central broker, or a single binary doing both, which is what a FIWARE@Home
    installation on a Raspberry Pi actually wants. Same codebase either way,
    selected by the conditional compilation above.

    See [Speaking to devices directly](device-protocols.md) for the deployment
    shapes, the transport × payload split that keeps the plugin count down, and
    what parity with the existing FIWARE IoT Agents requires. Lands after the
    binary IPC protocol and corDB.

## Long term

The following are proposals regarding the longer-term evolution of the product.
Take into account that there is no commitment to deliver them in a specific
timeframe; they are provided so that potential contributors can see where the
product is heading and may wish to get involved.

-   **Array reduction in `corJsonld`.** A single JSON-LD normalisation applied once
    at the input boundary rather than at each call site.

-   **Embedded deployment.** The broker is under 1 MiB and starts in 10 ms; running
    it on constrained hardware is a question of build configuration, not redesign
    — the per-feature `#ifdef`s and the choice of HTTP implementation are what
    make that true rather than aspirational.

-   **Continuous ETSI conformance.** Keeping the official test suite at 100% as the
    specification evolves, and feeding test-side corrections upstream.

-   **Broader performance regression coverage.** Measured nightly and recorded, so
    a regression is noticed by CI rather than by a user.
