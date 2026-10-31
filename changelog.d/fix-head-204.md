### Changed behaviour

- A HEAD request on the NGSI-LD API answers `204 No Content` when the GET would succeed, as TS 104-176 § 5
  requires ("The response code 204 No Content shall be used on success"). The GET's headers stay (`Link`,
  `NGSILD-Results-Count`, `Location`), and a 204 has no `Content-Length`. An error keeps the GET's status, with
  no body. Was `200 OK` with the GET's `Content-Length`. A HEAD on the broker's own endpoints (`/version`, ...)
  still answers what their GET does.

### Fixed

- The `Allow` header of a 405 listed `HEAD` on a resource without a GET: `GET
  /temporal/entities/{id}/attrs/{attrId}` answered `Allow: DELETE, HEAD, OPTIONS`, now `DELETE, OPTIONS`.
