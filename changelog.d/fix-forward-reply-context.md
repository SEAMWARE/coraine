### Fixed

- A context source that answers a forwarded retrieve or query in plain JSON - no `Link` header, no
  `@context` in the body - is read in the `@context` the request was forwarded with, not the core context.
  Its attributes and type came back as `ngsi-ld:default-context/...` instead of the client's short names.
  A source registered with a `jsonldContext` (`contextSourceInfo`) is such a source.
