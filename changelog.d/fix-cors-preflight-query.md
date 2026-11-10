### Fixed

- A CORS preflight (`OPTIONS`) of a URL with a query is answered (204, with the CORS headers): it was
  refused 400 "Unknown/unsupported URL parameter", and a browser never sent the request - a batch upsert
  with `?options=replace`, a `GET` with `?type=...` and a non-simple header. A preflight checks no URL
  parameter; the request itself checks its own (corRest).
