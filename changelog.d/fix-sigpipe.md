### Fixed

- A peer that goes away while the broker writes to it - an HTTP client, a WebSocket, a notification
  receiver - no longer ends the broker. The write raised SIGPIPE, whose default action ended the process:
  the broker now ignores SIGPIPE, and the write fails with EPIPE where it is made. Seen on a WebSocket
  closed while the broker sent its close frame at shutdown.
