### Fixed

- With an auth plugin (`--auth oidc`, `--auth fdsc`) and `--corsOrigin`, a browser's CORS preflight (`OPTIONS`
  with `Origin` and `Access-Control-Request-Method`) was a 401: a browser never sends credentials on a
  preflight, so no web app on another origin could reach the broker. A preflight is now answered without a
  token; the request itself still needs one. An `OPTIONS` that is no preflight is checked as before.
