### New

- `coraine --https --key <file> --cert <file>`: the NGSI-LD API over HTTPS (TLS), on its `--port` - the
  server's private key and certificate are PEM files, read at start; the default `--httpEndpoint` then
  begins with `https://`. libmicrohttpd does the TLS: a build with the built-in HTTP server refuses
  `--https`.
  ([Command-line options](doc/options.md#--https))
