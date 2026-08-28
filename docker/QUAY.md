# Coraine — NGSI-LD Context Broker

A lightweight NGSI-LD context broker written in C, **fully implementing ETSI GS CIM 009
v1.9.1** and passing the official ETSI NGSI-LD conformance test suite with a 100%
success rate.

The aim is to be current with the specification at all times, not current with the last
release of it: anything added to the next draft is implemented as it appears there,
rather than after it is published. A draft can change, and then it is implemented again —
that is the cost of being early, and it is a cost worth paying.

Plugin-driven: storage backends, temporal history and extra API surfaces are all `.so`
plugins chosen at startup.

© Seamware.

---

## Quick start — no external services

The broker ships with an in-memory storage backend, so a single container is a complete,
working NGSI-LD endpoint. Nothing to install, nothing to connect:

```sh
docker run --rm -p 1026:1026 quay.io/seamware/coraine:0.4.0 --database corDB
```

```sh
curl localhost:1026/ngsi-ld/v1/types

curl -X POST localhost:1026/ngsi-ld/v1/entities \
     -H 'Content-Type: application/json' \
     -d '{"id":"urn:ngsi-ld:Sensor:1","type":"Sensor",
          "temperature":{"type":"Property","value":21.5}}'

curl localhost:1026/ngsi-ld/v1/entities/urn:ngsi-ld:Sensor:1
```

Data lives in memory only and is gone when the container stops — usually what you want for
interop testing, since every run starts from a clean state.

## With MongoDB — persistent storage

`mongoc` is the default backend. `--dbHost` defaults to `localhost`, which inside a
container means the container itself, so it has to be pointed at the database:

```sh
docker network create ngsild
docker run -d --name mongo --network ngsild mongo:8
docker run -d --name broker --network ngsild -p 1026:1026 \
    quay.io/seamware/coraine:0.4.0 \
        --database mongoc --dbHost mongo --dbName cor
```

## Common options

`--usage` lists everything, including the arguments of whichever plugins are loaded:

```sh
docker run --rm quay.io/seamware/coraine:0.4.0 \
    --database mongoc --apiPlugins admin --usage
```

- `--port` / `-p` — TCP listen port (default 1026)
- `--database` / `-db` — storage plugin: `mongoc` (default) or `corDB`
- `--troe` / `-troe` — temporal history plugin: `none` (default), `ramdb`, `timescale`
- `--apiPlugins` / `-api` — extra API plugins, comma-separated; `admin` adds ops endpoints
- `--pretty-print` / `-pp` — JSON indentation (0 = compact)
- `--distributed` / `-dist` — enable distributed operations (off by default; see below)
- `--defaultUserContext` / `-duc` — default `@context` URL
- `--corsOrigin` — enable CORS (`__ALL` for any origin)
- `--maxRequestSize` / `-mrs` — max body in MiB (0 = no cap)
- `--dbHost`, `--dbPort`, `--dbName`, `--dbUser`, `--dbPwd`, `--dbURI` — MongoDB connection

`-fg` (foreground) is already in the entrypoint; without it the broker would daemonize and
the container would exit immediately.

## Distributed operations

Answering from *other* brokers (Context Source Registrations) is **off by default** —
switch it on with `--distributed`:

```sh
docker run -d --name b1 --network ngsild -p 1026:1026 \
    quay.io/seamware/coraine:0.4.0 --database corDB --distributed
```

Without it, registrations are still stored and discoverable, but nothing is ever
forwarded. Only the broker that forwards needs the flag; the one holding the data
does not.

Two things to know when a forward doesn't happen — federation failures are silent by
design, so a dropped forward looks like `200` with an empty result:

- **The registration's `endpoint` must be resolvable from the broker**, not from your
  shell. Inside a container, `localhost` is that container.

- **`--httpEndpoint`** is the address the broker advertises as its own. It is
  auto-detected from the interfaces, which is fine for loop detection (each container
  gets its own address), but the guess is container-internal — so set it whenever peers
  need to reach *this* broker at a routable address, i.e. for distributed subscriptions
  and the callbacks and `Link` headers it hands out.

Loop detection identifies a broker by its `contextSourceAlias`, derived from that
endpoint's authority. Two brokers that somehow end up with the *same* alias each take
the other for themselves and refuse to forward; `--csourceAlias` sets it explicitly.

To see why a registration was or wasn't forwarded to, run with `-t 227`:

```
urn:ngsi-ld:CSR:b2: MATCH
urn:ngsi-ld:CSR:b2: matched, but NOT forwarded to: loop — its alias 'broker:1026' is our own
```

## Which build am I running?

```sh
curl localhost:1026/ngsi-ld/v1/info/sourceIdentity
```

The response carries the version, the exact git commit and the build timestamp under
`contextSourceExtras` — quote the `gitSha` in any bug report.

## Ops endpoints

With `--apiPlugins admin`, Prometheus metrics are exposed:

```sh
curl localhost:1026/admin/metrics
```

## Healthcheck

The image healthchecks itself against `/ngsi-ld/v1/types`, which needs no parameters and no
data to answer. With the `mongoc` backend that endpoint reads the database, so the container
correctly reports unhealthy when its MongoDB is unreachable.

## Tags

Two kinds, and which you want depends on why you are pinning.

| Tag | Example | Published by |
|---|---|---|
| **Release** — the bare version | `0.4.0` | cutting a release, once per version |
| **Per-merge** — `<version>-<date>-<commit>` | `0.4.0-2026-08-28-f81859a` | every merge to `main` |

**Use the release tag** unless you have a reason not to: it is the version the
[release notes](https://github.com/SEAMWARE/coraine/releases) describe, and it is
written once and never moved. The per-merge tags are for pinning something newer
than the last release, or for identifying exactly which build you ran.

There is deliberately **no `latest`** and no bare-commit tag. A tag that moves
under a running deployment is a version nobody can name afterwards, and a tag
that expires turns a working `docker run` in someone's notes into a
`manifest unknown` months later.

Whichever you pick, the broker can tell you what it is: `GET /version` reports
its own version *and* the resolved commit of every library linked into it, which
is most of the binary by volume.
