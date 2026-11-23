### New

- EntityMaps on Retrieve Entity, Retrieve Attribute and Retrieve Attribute value (TS 104-175 § 10.4.2.4,
  TS 104-176 tables 7.3.3.1-1, 7.5.3.1-1, 7.6.3.1-1). `?entityMap=true` makes a map of the entity, with the
  sources that answered with it, and answers 201 with the map in `NGSILD-EntityMap`. The `NGSILD-EntityMap`
  request header names a map to use: only the registrations its entry for the entity names are asked, each
  sent its own map (the linked map) in `NGSILD-EntityMap`, and the maps the sources used become the map's
  linked maps; 200, the map in `NGSILD-EntityMap`. A named map that is gone is made anew: 201.
- `entityMapLifetime` (an ISO 8601 duration) on Query Entities, Query Temporal Evolution of Entities and the
  three retrievals above, and in the Query of `POST /entityOperations/query` and
  `POST /temporal/entityOperations/query`: the lifetime of the map `entityMap=true` (`createEntityMap`) makes.
  Without it a map lives 5 minutes, as before.

### Changed behaviour

- A request that makes an EntityMap in place of one that is gone (the `NGSILD-EntityMap` request header, or
  a page link's `entityMap=<id>`, naming an expired or deleted map) answers 201, as TS 104-176 table
  7.2.3.1-2 gives for a map "(re)created"; it answered 200.
- Query Temporal Evolution of Entities with `?entityMap=true` (GET, and `POST /temporal/entityOperations/query`)
  answers 201 (TS 104-176 table 8.2.3.1-2); it answered 200.

### Fixed

- `entityMapLifetime` was refused as an unknown URL parameter (400) on every operation.
- `entityMap` with a value that is neither `true`, `false` nor the id of a map (a URI) was taken as the id of
  a map that does not exist - a new map was made and the query answered 200. It is 400 BadRequestData.
- `POST /entityOperations/query` with `"createEntityMap": true` was 400 "Invalid field for query": the v1.9
  core context has no term for it, so it came out of the expansion in the default vocabulary. It makes a map,
  201. The same for `firstN` and `offsetN` in the `temporalQ` of `POST /temporal/entityOperations/query`:
  they were ignored.
