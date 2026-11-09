### Fixed

- A distributed Query Temporal Evolution of Entities sent as a POST (`/temporal/entityOperations/query`)
  reaches the Context Sources with its query: the entities, `temporalQ`, `attrs`, `q` and the geo-query
  of the body. Before, only the URL's parameters were forwarded, a source got none of the body's - and
  answered nothing, so the response held the local entities only.
