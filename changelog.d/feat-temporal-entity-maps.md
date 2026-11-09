### New

- `GET`, `HEAD` and `POST /ngsi-ld/v1/temporal/entityMaps` - Create EntityMap for Query Temporal Evolution of
  Entities (TS 104-176 § 11.3, TS 104-175 § 14.6): the ids of the entities a temporal query selects, frozen
  in an EntityMap, with the maps of the Context Sources whose registrations list
  `createEntityMapQueryTemporal`. 201, the map and `NGSILD-EntityMap`, as `GET /entityMaps` answers.
  ([EntityMaps](doc/installation.md#the-entitymap-of-a-temporal-query))
