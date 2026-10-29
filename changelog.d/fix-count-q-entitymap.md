### Fixed

- A query with `q`, `scopeQ` or a geo-query over a registered Context Source holding a page or more of
  matching entities (20 by default) - the query an automatic EntityMap pages - counted and paged every entity
  of the type, whatever the filter said: `NGSILD-Results-Count` was the number of entities of the type, not
  of matches, and a page was cut before the filter, so `limit=1` with a match further on answered no rows.
  The rows were right. With split entities (the default) the sources cannot be given the filters; the
  broker now applies them to the entities it assembles when the EntityMap is created, so the map holds the
  matches only. ([EntityMaps](doc/installation.md#entitymaps))
