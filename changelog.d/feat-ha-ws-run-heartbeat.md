### New

- Under `--high-availability mongo`, the WebSocket subscriptions of an instance that was killed or that
  crashed are deleted by the instances alive, also when it never comes back. Each instance writes the
  beat of its run in the shared database every 10 s; the subscriptions notifying a connection of a run
  with no beat for 60 s are deleted. An instance whose beat cannot be written for 60 s closes its
  WebSocket connections (status 1012). Build switch `COR_FEATURE_HA_RUNS`.
  ([High Availability](doc/high-availability.md#websocket-subscriptions-of-an-instance-gone))

### Changed behaviour

- The transport plugin ABI is 2: a driver has a `drop` operation (every connection closed, with a status).
  A transport plugin built for ABI 1 is still loaded, without it.
