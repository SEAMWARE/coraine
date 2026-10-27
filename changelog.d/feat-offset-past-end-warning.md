### New

- A query with an `offset` at or past the end of its result set answers its last entity, as before, and now
  says so: `NGSILD-Warning: 199 coraine "offset 50 is past the result set (5 entities) - the last entity is
  returned"`.
  ([Pagination](doc/installation.md#an-offset-past-the-end))

### Fixed

- The `prev` link of a page past the end pointed before the offset asked for - past the end too (`offset=10`,
  5 entities, `limit=5`: `prev` at `offset=5`); it is the page before the last entity.
- An EntityMap page with an `offset` past the end of the map was empty (`[]`); it is the map's last entity, as
  for a query.
