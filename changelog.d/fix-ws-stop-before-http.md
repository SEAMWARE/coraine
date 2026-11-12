### Fixed

- A broker stopped with WebSocket connections open could crash on its way out (SIGSEGV): the HTTP server was
  stopped first, freeing the connections' handles, and each connection's thread then handed back a freed one.
  The connections are now closed first - the client gets a close frame `1001` (going away), which it did not
  before - and a WebSocket asked for during the stop is refused.
