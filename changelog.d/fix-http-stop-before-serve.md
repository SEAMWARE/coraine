### Fixed

- A broker built with the built-in HTTP server (`COR_HTTP_SERVER=builtin`) could hang on SIGTERM / SIGINT that
  arrived right after it started serving: an HTTP event loop that had not started yet missed the stop and
  kept running, and the broker waited for it forever. (corHttp)
