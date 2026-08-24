# API walkthrough

Everything below runs against a broker started with no external service:

```sh
coraine --database corDB --troe none --apiPlugins admin -pp 2
```

The examples use port 1026 and `curl`. NGSI-LD lives under `/ngsi-ld/v1`.

## 1. Create an entity

```sh
curl -X POST http://localhost:1026/ngsi-ld/v1/entities \
  -H 'Content-Type: application/json' \
  -d '{
        "id": "urn:ngsi-ld:Vehicle:A100",
        "type": "Vehicle",
        "brand":   { "type": "Property",     "value": "Mercedes" },
        "speed":   { "type": "Property",     "value": 80, "observedAt": "2026-08-20T10:00:00Z" },
        "isParked":{ "type": "Relationship", "object": "urn:ngsi-ld:OffStreetParking:P1" }
      }'
```

`201 Created`, with a `Location` header naming the entity. A `Content-Type` of
`application/json` means the `@context` comes from a `Link` header, or, absent one,
the NGSI-LD core context.

## 2. Retrieve it

```sh
curl http://localhost:1026/ngsi-ld/v1/entities/urn:ngsi-ld:Vehicle:A100
```

Three representations are available, chosen with `?format=`:

```sh
curl '.../entities/urn:ngsi-ld:Vehicle:A100?format=normalized'   # the default - full attribute objects
curl '.../entities/urn:ngsi-ld:Vehicle:A100?format=concise'      # type omitted where it can be inferred
curl '.../entities/urn:ngsi-ld:Vehicle:A100?format=keyValues'    # values only
```

## 3. Query

```sh
# by type
curl 'http://localhost:1026/ngsi-ld/v1/entities?type=Vehicle'

# by a filter on attribute values (§ 4.9)
curl 'http://localhost:1026/ngsi-ld/v1/entities?type=Vehicle&q=speed>50'

# only some attributes, and how many there are in total
curl 'http://localhost:1026/ngsi-ld/v1/entities?type=Vehicle&attrs=speed&count=true'
```

`count=true` puts the total in the `NGSILD-Results-Count` response header, and
pagination is `limit` and `offset`, with `Link` headers for the next and previous
pages.

## 4. Update

```sh
# append or overwrite attributes
curl -X POST http://localhost:1026/ngsi-ld/v1/entities/urn:ngsi-ld:Vehicle:A100/attrs \
  -H 'Content-Type: application/json' \
  -d '{ "colour": { "type": "Property", "value": "black" } }'

# merge-patch the entity (§ 5.6.16)
curl -X PATCH http://localhost:1026/ngsi-ld/v1/entities/urn:ngsi-ld:Vehicle:A100 \
  -H 'Content-Type: application/json' \
  -d '{ "speed": { "type": "Property", "value": 95 } }'

# one attribute
curl -X PATCH http://localhost:1026/ngsi-ld/v1/entities/urn:ngsi-ld:Vehicle:A100/attrs/speed \
  -H 'Content-Type: application/json' \
  -d '{ "type": "Property", "value": 100 }'
```

## 5. Subscribe

A subscription asks for a notification whenever a matching entity changes:

```sh
curl -X POST http://localhost:1026/ngsi-ld/v1/subscriptions \
  -H 'Content-Type: application/json' \
  -d '{
        "id": "urn:ngsi-ld:Subscription:S1",
        "type": "Subscription",
        "entities": [ { "type": "Vehicle" } ],
        "watchedAttributes": [ "speed" ],
        "q": "speed>90",
        "notification": {
          "attributes": [ "speed", "brand" ],
          "format": "keyValues",
          "endpoint": { "uri": "http://localhost:9977/notify", "accept": "application/json" }
        }
      }'
```

Anything with an HTTP endpoint can receive them; MQTT endpoints
(`mqtt://host:port/topic`) work the same way. The subscription is retrievable,
patchable and deletable at `/ngsi-ld/v1/subscriptions/{id}`.

## 6. Delete

```sh
curl -X DELETE http://localhost:1026/ngsi-ld/v1/entities/urn:ngsi-ld:Vehicle:A100
```

## Where to go next

- **Distributed operations** — register another broker or Context Source as a source
  of some entities, and coraine forwards to it (`--distributed`, and
  `/ngsi-ld/v1/csourceRegistrations`).
- **Temporal** — with a temporal plugin (`--troe timescale`), the history of every
  attribute is queryable under `/ngsi-ld/v1/temporal/entities`.
- **The full API** — coraine implements ETSI GS CIM 009 v1.9.1 in its entirety; the
  specification is the reference, and the
  [plugin architecture](plugin-architecture.md) explains how to extend the broker
  itself.

  Temporal writes are DEFERRED until after the response: a temporal write is
  history, not state the caller is waiting on, so the request pays nothing for
  it. The consequence is that a `201` does not mean the temporal row is
  queryable yet - create an entity and read its temporal representation in the
  same breath and it may not be there. Start the broker with `--troeSync` when
  you want read-your-writes instead; writes then pay the storage latency inline.
