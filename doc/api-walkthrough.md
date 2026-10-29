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

What goes with `q` says how its names and values are read: `expandValues` (the
values of these attributes are expanded with the `@context`), `jsonKeys` (the
`[..]` under these attributes are JSON members, as sent) and `langProperties`
(these attributes are LanguageProperties: the `[..]` under them is a language tag,
compared as is with the `languageMap` keys - not NGSI-LD yet). In a URL each is a
comma separated list; in a POST Query body and a Subscription each is a string,
comma separated, or an array of strings with one name per item. A name is short
or already expanded.

```sh
# [fr] a language tag - urn:ngsi-ld:Street:1, whose name is {"fr": "Grand Place", "nl": "Grote Markt"}
curl 'http://localhost:1026/ngsi-ld/v1/entities?type=Street&q=name%5Bfr%5D==%22Grand%20Place%22&langProperties=name'

# the same in a POST Query body, the list an array
curl http://localhost:1026/ngsi-ld/v1/entityOperations/query -H 'Content-Type: application/json' \
  -d '{"type": "Query", "entities": [{"type": "Street"}], "q": "name[nl]==\"Grote Markt\"", "langProperties": ["name"]}'
```

Without `langProperties`, `[fr]` is a term - a member of a compound value - and the
query above finds nothing.

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
(`mqtt://host:port/topic`, `mqtts://...`) work the same way once the broker is started
with the MQTT bridge, `--bridges mqtt` - without it such a subscription is refused. The subscription is retrievable,
patchable and deletable at `/ngsi-ld/v1/subscriptions/{id}`.

With `"accept": "application/geo+json"` a notification's `data` is a GeoJSON FeatureCollection, each
Feature's `geometry` the value of the entity's `location`. `notification.geometryProperty` names another
GeoProperty for it, as the `geometryProperty` URL parameter does for a query - proposed for the spec in
TS 104 175 issue #67, an extension until it is adopted:

```sh
curl -X POST http://localhost:1026/ngsi-ld/v1/subscriptions \
  -H 'Content-Type: application/json' \
  -d '{
        "type": "Subscription",
        "entities": [ { "type": "Drone" } ],
        "notification": {
          "geometryProperty": "position",
          "endpoint": { "uri": "http://localhost:9977/notify", "accept": "application/geo+json" }
        }
      }'
```

Without `"accept": "application/geo+json"` a `geometryProperty` is refused (400 BadRequestData), as a
query's is without a GeoJSON `Accept`.

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
  you want read-your-writes instead: the history is handed over before the
  response (not committed - nothing waits for the database), and a temporal read
  waits for what was handed over before it. Writes pay 1-6 % for it.
