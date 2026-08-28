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
- **Written in C**, built with CMake. One process, no runtime, no JVM.

### Plugins, loaded at startup as shared libraries

- **Current state** — `mongoc` (MongoDB) and `corDB` (in-memory).
- **Temporal history** — `timescale` (TimescaleDB), `ramdb` (in-memory) and `none`.
- **API surfaces** — `admin`, for the endpoints that are not NGSI-LD: ops and
  administration, and **55 Prometheus metrics** on `/metrics` and
  `/admin/metrics` covering entity, batch, subscription, registration, context
  source and distributed-operation activity.

Which ones a build contains is a compile-time choice, and which ones it loads is a
run-time one.

### What a release now pins

- `GET /version` reports the resolved commit of **every library linked into the
  broker** — the seven k-libs and the four Cor-Libs — not only coraine's own
  version. Most of the binary is library code, and the repositories it comes from
  track `main`.
- Cutting this tag stamps every Cor-Lib with `v0.4.0` at the exact commit this
  release was built from, so the stack behind a release stays recoverable after
  those repositories move on.

### Fixed

- **`/metrics` was not valid Prometheus exposition format.** The render buffer was
  allocated at exactly `kpromRenderSize()`, which reports the payload length
  *excluding* the terminating NUL — so the final byte, the newline ending the last
  metric, was overwritten with a NUL that `Content-Length` then counted. The
  payload was binary rather than text, and a scraper would have rejected it.

### Known limitations

- **HTTP is the only transport.** The *bridge* seam that lets a subscription or a
  registration name a different one by the scheme of its endpoint is designed and
  not built, and so is the binary IPC protocol beside REST.
- **Actuation is workflow, not API.** Service Execution — actuation as a
  first-class citizen rather than the suggested workflows of TS 104 175 Annex G —
  is short-term work, not present here.
- **No packages.** The container image or a build from source; there is no
  `apt-get install coraine` yet.
- **On the 100%.** Conformance is measured against a corrected fork of the ETSI
  suite. The corrections are test-side — bugs in the suite as published, parts of
  which do not run — and are filed upstream with ETSI. They never relax what the
  broker must do.

The [roadmap](https://coraine.readthedocs.io/en/latest/roadmap/) has the rest.
