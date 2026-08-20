# coraine roadmap

This is the public roadmap. It is deliberately short: the detailed, current backlog —
including what is deferred *by design* and why — lives in
[`ToDo.md`](../ToDo.md) at the root of the repository, and is kept up to date as work
lands rather than at release boundaries.

## Where coraine is today

- **NGSI-LD ETSI GS CIM 009 v1.9.1 fully implemented**, passing the official ETSI
  conformance test suite.
- Plugin-driven: current-state DB, temporal (TRoE) and extra API surfaces are shared
  libraries chosen at startup.
- Two DB backends (`mongoc`, `corDB`), three temporal backends (`none`, `ramdb`,
  `timescale`), one API plugin (`admin`).

## Short term

- **Service Execution** — actuation as a first-class API citizen, beyond the
  suggested workflows of TS 104 175 Annex G.
- **Communication protocols as plugins** — lift REST/HTTP behind the same plugin seam
  as the DB and TRoE drivers, then add a binary IPC protocol beside it.
- **Finish conditional compilation** — per-feature `#ifdef`s so a deployment compiles
  only the NGSI-LD it uses.

## Medium term

- **DDS** and **OPC UA** as transports, addressed as `local://<name>` endpoints, for
  the robotics and industrial-automation side.
- **WebSockets** — notification delivery to consumers that cannot be HTTP servers.
- **corDB** — an NGSI-LD-aware store with entities cached in RAM.
- **haaux** — high-availability cache synchronisation without a shared database.

## Continuous

- ETSI conformance: keeping the suite at 100% as the specification evolves, and
  feeding test-side corrections back upstream.
- Growing functional-test coverage — the gap list is
  [`doc/spec-coverage-gaps.md`](spec-coverage-gaps.md), the measured numbers are in
  [`doc/coverage.md`](coverage.md).
