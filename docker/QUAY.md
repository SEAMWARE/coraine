# NGSI-LD Context Broker

A lightweight NGSI-LD context broker written in C, targeting **ETSI GS CIM 009 v1.9.1**.
Plugin-driven: storage backends, temporal history and extra API surfaces are all `.so`
plugins chosen at startup.

© Seamware.

---

## Quick start — no external services

The broker ships with an in-memory storage backend, so a single container is a complete,
working NGSI-LD endpoint. Nothing to install, nothing to connect:

```sh
docker run --rm -p 1026:1026 quay.io/seamware/broker --database swRamDB
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
    quay.io/seamware/broker --database mongoc --dbHost mongo --dbName sw
```

## Common options

`--usage` lists everything, including the arguments of whichever plugins are loaded:

```sh
docker run --rm quay.io/seamware/broker --database mongoc --apiPlugins admin --usage
```

- `--port` / `-p` — TCP listen port (default 1026)
- `--database` / `-db` — storage plugin: `mongoc` (default) or `swRamDB`
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

A broker that must answer from *other* brokers (Context Source Registrations) needs
three things right. Miss any of them and queries come back `200` with an empty result
and nothing in the log — federation failures are silent by design.

```sh
docker run -d --name b1 --network ngsild -p 1026:1026 \
    quay.io/seamware/broker --database swRamDB \
    --distributed --httpEndpoint http://192.168.1.10:1026
```

1. **`--distributed`** — forwarding is **off by default**; without it registrations are
   still stored and discoverable, but nothing is ever forwarded. Only the broker that
   forwards needs the flag; the one holding the data does not.

2. **`--httpEndpoint`** — the address the broker believes is its own, auto-detected from
   the host's interfaces when not given. Inside a container that guess is a
   container-internal address. Set it to the address *peers* use to reach this broker.

3. **Distinct identities.** Loop detection compares a `contextSourceAlias`, which defaults
   to the authority of `--httpEndpoint`. Several brokers that end up with the *same* alias
   each take the others for themselves and refuse to forward. This bites when brokers run
   as containers that all bind the same internal port and differ only in the published
   port — give each one its own `--httpEndpoint` (or an explicit `--csourceAlias`).

The registration's `endpoint` must be resolvable **from the broker**, not from your shell:
inside a container `localhost` is that container.

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

- `latest` — most recent build
- `<gitSha>` — the exact commit the image was built from
