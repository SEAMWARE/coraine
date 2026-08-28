# Changelog

Notable changes per release. The entry for a version is what its
[GitHub release](https://github.com/SEAMWARE/coraine/releases) page shows, so it is
written for someone deciding whether to upgrade — not a copy of the git log.

Versions follow [semantic versioning](https://semver.org/). Below 1.0.0 a minor
bump may still change behaviour; the entry says so where it does.

---

## 0.4.0 — 2026-08-28

The first tagged release. Everything below already existed and had never carried a
version anyone could ask for by name — that is what this release is for, and why
the entry describes a surface rather than a diff.

### The broker

- **NGSI-LD, in full.** ETSI GS CIM 009 v1.9.1 — entities, subscriptions,
  registrations, batch operations, the temporal API, distributed operations across
  context sources, and JSON-LD `@context` handling.
- **Notifications over HTTP, HTTPS, MQTT and MQTTS.** MQTT QoS and protocol
  version are selectable per subscription through `endpoint.notifierInfo`
  (§ 7.2), and the MQTT broker may require TLS, a username and a password.
- **Written in C**, built with CMake. One process, no runtime, no JVM.

### Plugins, loaded at startup as shared libraries

- **Current state** — `mongoc` (MongoDB) and `corDB` (in-memory).
- **Temporal history** — `timescale` (TimescaleDB), `ramdb` (in-memory) and `none`.
- **API surfaces** — `admin`, for the endpoints that are not NGSI-LD: ops and
  administration, and **56 Prometheus metrics** on `/metrics` and
  `/admin/metrics` covering entity, batch, subscription, registration, context
  source and distributed-operation activity.

Which ones a build contains is a compile-time choice, and which ones it loads is a
run-time one.

Where coraine goes from here is on the
[roadmap](https://coraine.readthedocs.io/en/latest/roadmap/).
