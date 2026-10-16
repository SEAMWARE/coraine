# Timepoint - an entity as it was at a point in time

The timepoint resources (NGSI-LD v2.1, ETSI DATAINFRA(26)000027) are the Core API's retrieve and query,
evaluated at a point in time: the entities as they were at `timeAt`. The answer is a regular Entity, or an
Entity array - never the temporal representation of `/temporal/entities`.

| Resource | Operation | Core API counterpart |
|---|---|---|
| `GET /ngsi-ld/v1/temporal/timepoint/entities/{entityId}` | retrieveEntityTimepoint | `GET /entities/{entityId}` |
| `GET /ngsi-ld/v1/temporal/timepoint/entities` | queryEntityTimepoint | `GET /entities` |
| `POST /ngsi-ld/v1/temporal/timepoint/entityOperations/query` | queryEntityTimepoint | `POST /entityOperations/query` |

## timeAt

Mandatory: a missing or invalid `timeAt` is a 400 BadRequestData.
- `GET`: the URL parameter `timeAt`.
- `POST`: a member of the Query - `{ "type": "Query", ..., "timeAt": "..." }`. Not of a `temporalQ`: a
  `temporalQ` selects entities by their history (`timerel`, its own `timeAt`), while the timepoint's `timeAt`
  picks the instance of each attribute an entity had then. A timepoint query with a `temporalQ` is a 400,
  and so is a Core API POST query with a `timeAt` member: a Query has none.

`timeAt` is on system time: the entity as the broker held it at `timeAt` - each attribute's last write at
or before it. An entity created after `timeAt`, or deleted at it, is not there (404 for a retrieve). An
attribute deleted before `timeAt` is not in the entity. An entity deleted and created again answers with
the life `timeAt` is in: the attributes of an earlier life do not come back.

## Everything else is the Core API's

Its parameters, their meaning, its rendering and its errors: `q`, `geoQ`, `scopeQ`, `type`, `ids`,
`idPattern`, `pick`, `omit`, `attrs`, `datasetId`, `lang`, `options` (concise, keyValues, sysAttrs),
`limit`, `offset`, `count`, ... - evaluated on the entities as they were. A `q` on `speed` filters on the
speed at `timeAt`; `count` counts the entities that matched then.

Not on the timepoint resources (400, an unknown parameter): `join` and `joinLevel` (the linked entities
would be read as they are now), `entityMap` (a map's pages read the entities as they are now). A timepoint
request is not served from a Snapshot (`NGSILD-Snapshot`: 400), and creates no automatic EntityMap.

## Where the history comes from

From the TRoE: `--troe corDB` (the history inside the corDB store, `--database corDB`). A TRoE without the
timepoint reads (`--troe none`, `--troe ramDB`, `--troe timescale` today) answers as for any temporal
operation it lacks. The entity's types are its current ones - a change of types is not kept in the history.
A timepoint request is local: no registration's source is asked.

## Build

`COR_FEATURE_TIMEPOINT` (ON by default; OFF removes the resources - about 2.4 KB of the broker).
