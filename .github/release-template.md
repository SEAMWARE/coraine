A lightweight **NGSI-LD Context Broker** written in C, fully implementing
**ETSI GS CIM 009 v1.9.1** and passing the official ETSI NGSI-LD conformance test
suite with a **100% success rate**.\*

What makes it worth a look:

- **Small.** **4.3 MiB** of files a machine did not already have — the broker,
  its in-memory store and three shared libraries. One process, no runtime, no
  JVM, and no database server to install beside it.
- **Fast.** **6 588 requests/s per core** — which at 20 entities a response is
  **131 760 entities/s per core** — and answering 9 ms after `exec`. Writes:
  42 234 entities/s per core one at a time, 131 260 in batches of twenty.
- **Plugin-driven.** Storage backend, temporal history and extra API surfaces are
  shared libraries loaded at startup. The core speaks NGSI-LD; the plugins decide
  where data lives and how the broker talks to the world.

## Getting started

An in-memory store, no external services — the broker answers NGSI-LD on 1026:

```sh
docker run --rm -p 1026:1026 \
    quay.io/seamware/coraine:{{VERSION}} --database corDB
```

Then, from another terminal:

```sh
curl -X POST localhost:1026/ngsi-ld/v1/entities \
     -H 'Content-Type: application/json' \
     -d '{ "id": "urn:ngsi-ld:Sensor:1", "type": "Sensor",
           "temperature": { "type": "Property", "value": 21.4 } }'

curl localhost:1026/ngsi-ld/v1/entities/urn:ngsi-ld:Sensor:1
```

For a persistent store, point it at MongoDB with `--database mongoc` instead. The
[README](https://github.com/SEAMWARE/coraine#readme) walks through the rest.

## Where to look next

- **[Documentation](https://coraine.readthedocs.io/en/latest/)** — installation
  and administration, the API walkthrough, the plugin architecture, building from
  source, and the roadmap.
- **[Container images](https://quay.io/repository/seamware/coraine?tab=tags)** —
  every published tag. `{{VERSION}}` is this release; every merge to `main` also
  publishes an immutable `<version>-<date>-<commit>`.
- **[Issues](https://github.com/SEAMWARE/coraine/issues)** — questions, bugs and
  feature requests all belong here; that is where the maintainers are.
- **[README](https://github.com/SEAMWARE/coraine#readme)** — footprint and
  throughput numbers, the plugin catalogue, and how the test suites are run.

---

## What is in {{VERSION}}

{{CHANGELOG}}

---

\* The conformance runs use a *corrected fork* of the ETSI test suite. The changes
are test-side fixes — the suite has bugs of its own and parts of it do not run as
published — never relaxations of what the broker must do. The fixes are filed
upstream with ETSI.

Licensed under the [Apache License 2.0](https://github.com/SEAMWARE/coraine/blob/main/LICENSE).
