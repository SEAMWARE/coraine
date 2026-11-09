### New

- Query Temporal Evolution of Entities with an EntityMap (TS 104-176 table 8.2.3.1-1): `entityMap=true` on
  `GET /temporal/entities` and `POST /temporal/entityOperations/query` makes a map and answers its first page;
  `entityMap=<id>` (or the `NGSILD-EntityMap` request header) answers a page of a map - each entity's history
  from where the map says it is, a Context Source's by the registration the map recorded, in the map's time
  window and with its filters. A map made by `/temporal/entityMaps` is paged the same way.
  ([EntityMaps](doc/installation.md#the-entitymap-of-a-temporal-query))
