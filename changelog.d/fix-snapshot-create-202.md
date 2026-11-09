### Changed behaviour

- `POST /ngsi-ld/v1/snapshots` and `POST /ngsi-ld/v1/snapshots/{snapshotId}/clone` answer `202 Accepted`,
  as TS 104 176 § 13.2.3.1 and § 13.4.3.1 say - they answered `201 Created`. The `Location` header is
  unchanged.
  ([Command-line options](doc/options.md#--asyncsnapshot))

### Fixed

- A Snapshot whose `id` is not a URI is not created (400 BadRequestData), nor a clone whose `id` is not
  one; `GET`, `PATCH`, `DELETE` and `clone` on `/ngsi-ld/v1/snapshots/{snapshotId}` with an id that is not
  a URI answer 400 - they created it, or answered 404.
- A Snapshot with a `temporalQ` in a Query of `snapshotQueries`, or a Query of `snapshotTemporalQueries`
  without one, is not created (400 BadRequestData, TS 104 175 § 5.2.6.5.4) - it was.
