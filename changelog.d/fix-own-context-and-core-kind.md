### Changed behaviour

- The core context is listed by `GET /ngsi-ld/v1/jsonldContexts` as kind `Cached`, not
  `ImplicitlyCreated` (§ 5.13.1 - a local copy of the @context at its external URL, compiled in instead
  of downloaded). As every `Cached` @context, its metadata is served (`?details=true`) and its body is not:
  `GET /ngsi-ld/v1/jsonldContexts/<core URL>` answers 422 `OperationNotSupported`.

### Fixed

- A `Link` header (or an `@context` URL in a body) naming a @context the broker hosts, by the URL the
  broker gives it (`<--httpEndpoint>/ngsi-ld/v1/jsonldContexts/{id}`), is that context - read from the
  cache or the database. It was downloaded from the broker itself and cached a second time, as a `Cached`
  entry under the URL: `GET /jsonldContexts?kind=Cached` listed the Hosted context too.
