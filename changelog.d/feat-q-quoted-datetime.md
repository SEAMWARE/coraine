### Added

- q takes a DateTime with or without quotes. TS 104-175 § 7.2.3 is unclear on quotes, and a quoted
  `q=p.observedAt>"2021-01-01T00:00:00Z"` compared as a string, matching nothing. A quoted full DateTime is now
  compared by time against a value stored as a time (`observedAt`, `createdAt`, `modifiedAt`), and as a string
  against a string, as before. Date and Time values (no `T`) stay strings. Entity queries on mongoc, corDB and
  ramDB, and subscriptions; TRoE's `observedAt` already did.

### Fixed

- mongoc: a list on a timestamp (`q=p.observedAt==2026-01-01T00:00:00Z,2026-09-01T00:00:00Z`, also
  `createdAt` / `modifiedAt`) matched nothing; a DateTime list item was compared as its text with the stored
  time.
