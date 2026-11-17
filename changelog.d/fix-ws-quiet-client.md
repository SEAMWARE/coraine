### Fixed

- WebSocket: a client gone without a word - off the network, a proxy's stream dead - no longer keeps its
  connection and thread for good. A connection quiet for 30 s gets a ping; nothing from the client for three of
  them (90 s) closes it (1001). A browser answers pings by itself. The interval: `CORAINE_WS_PING` (seconds).
