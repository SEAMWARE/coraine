### New

- `GET /admin/probes` (the admin API plugin): what the code decided - which plan a query took - kept for the
  functional tests to read, in a release build too, where the traces are compiled out. Each probe set since the
  start or the last `DELETE /admin/probes`, and its last value; `GET /admin/probes/{name}` one of them.
  The first probe: `timescale.entitySelector` - `candidates` when a temporal query's `q` has a term an index serves.
  `COR_FEATURE_PROBES` (on): 2,237 bytes.

### Fixed

- `troe_timescale_db_indexes.test` failed against a release build: it read a trace of the broker's SQL, compiled
  out of one. It reads the probe `timescale.entitySelector` instead.
