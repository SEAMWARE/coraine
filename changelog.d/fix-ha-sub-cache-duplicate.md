### Fixed

- With `--high-availability mongo`, a subscription created on a broker could end up in that broker's cache
  twice, and notify twice: the create writes the subscription to the database and then adds it to the cache,
  and the change event of that very write, applied by the broker's own HA sync in between, had already added
  it. About one create in 300, more on a slower machine (seen on arm64, `ha_ws_run_gone`). A create now
  replaces a cached copy of the same id, under the same lock hold - entity subscriptions, subscriptions to
  registrations and periodic subscriptions (corNgsild `ldPernotCacheItemAdd`) alike. Registrations were not
  affected: their create holds the cache lock across the database write.
