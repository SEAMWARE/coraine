### Changed behaviour

- Purge Snapshots (`DELETE /snapshots?q=...`) whose q matches no Snapshot answers 404 ResourceNotFound
  (TS 104-176 table 13.2.3.2-2); it answered 204.

### Fixed

- Update Snapshot Status (`PATCH /snapshots/{id}`) refused a body carrying an output only member
  (`snapshotStatus`, `snapshotQueriesDetails`, `snapshotTemporalQueriesDetails`, `createdAt`, `modifiedAt`,
  `expiresAt`, `lastUsedAt`) with 400 "Immutable Field"; these are now ignored, as TS 104-175 § 5.2.6.5.4
  requires, and the rest of the body is applied. Create Snapshot ignores a client's `snapshotQueriesDetails` and
  `snapshotTemporalQueriesDetails` too - it kept them. `id`, `type`, `snapshotQueries` and
  `snapshotTemporalQueries` are still refused in an update.
- Purge Snapshots with a q naming something that is not a member of the Snapshot data type
  (`q=temperature==20`) answered 204; it is 400 BadRequestData (TS 104-175 § 16.7.4).
