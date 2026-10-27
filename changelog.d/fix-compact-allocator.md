### Fixed

- `GET /ngsi-ld/v1/csourceRegistrations` with a filter that drops a matching registration - `q`, `scopeQ`,
  `geoQ`, a temporal query, or none (a registration with an `observationInterval` or `managementInterval`
  is then left out) - released the matches wrongly: a dropped registration stayed held, so after its
  `DELETE` its memory was never freed, and a kept one was released twice, so a request forwarding to it
  while it was deleted could read freed memory. Each match is now released once.
