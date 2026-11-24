### New

- **Both HTTP servers in one broker**: `cmake -DCOR_HTTP_SERVER=both` (`coraine-build --http-server both`) builds a
  broker with libmicrohttpd and the built-in server, and `--httpServer mhd|builtin` (`CORAINE_HTTPSERVER`) picks the
  one it runs - libmicrohttpd by default. `coraine -V` and `GET /build` say the one running (`httpServer`) and the ones
  built (`httpServers`). A server the broker is not built with, and `--https` or `--policyApiPort` on the built-in
  server, stop the start ([The HTTP server](doc/installation.md#the-http-server)).
- **`coraine-build --list-http-servers [--json]`**: the `--http-server` values the source can build.
