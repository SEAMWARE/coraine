### Changed behaviour

- A 404 answer is logged as a warning (`W:`), not an error (`E:`) - an entity, subscription or resource that is not
  there is an answer, not a failure. Unknown resources (`GET /ngsi-ld/v1/ws` without the WebSocket transport) too.
  `GET /admin/log/lines?types=E` no longer lists them; `types=W` does. Other 4xx answers are still `E:`.
