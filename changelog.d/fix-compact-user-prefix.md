### Fixed

- An IRI expanded through a prefix of the request's `@context` (`ex:attr` with `"ex": "http://example.org/types/"`)
  is compacted back to that compact IRI when the response's `@context` has the prefix - an entity type, an
  attribute name, a `valueType` or `objectType` value. It was given as the full IRI. The core `@context`'s own
  prefixes are not used this way: an error type stays its IRI.
