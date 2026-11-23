### Changed behaviour

- `orderBy` on a query without `local=true` is 400 BadRequestData, title "Ordering outside the local scope"
  (TS 104-175 § 10.4.3.4, § 11.3.3.4): Query Entities and Query Temporal Evolution of Entities, GET and POST.
  A query on a Snapshot (`NGSILD-Snapshot`) forwards nothing and is ordered as before.
  ([Ordering and the local scope](doc/installation.md#ordering-and-the-local-scope))

### New

- `--orderDistributed` (`CORAINE_ORDERDISTRIBUTED`): accepts `orderBy` without `local=true`, and orders the merged
  local and Context Source matches - what coraine did by default before.
  ([--orderDistributed](doc/options.md#--orderdistributed))

### Fixed

- `orderBy` on `POST /ngsi-ld/v1/entityOperations/query` and the temporal POST query: the attribute path
  was expanded twice, giving 400 "Invalid Attribute Name".
