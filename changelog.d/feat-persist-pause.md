### New

- corDB with `--dbDir`: a log that cannot be written - a full disk, a file-size limit, a failed `fdatasync` -
  pauses the store. Every request that writes is refused before anything is applied: 503
  `PersistenceUnavailable`, `Retry-After: 5`; GET and POST `entityOperations/query` are served. `/ready`
  answers 503 and the health report's `status` is `paused`, with a `paused` member; metrics
  `ngsild_store_paused` and `ngsild_requests_refused_store_paused_total`. The log is allocated 8 MiB ahead of
  its records, so a full disk pauses the store before a write needs the room. The broker repairs by itself -
  a snapshot of each tenant from memory, retried with a backoff up to 16 s - and the pause ends once the disk
  has room again. A failure of the history log (`--troe corDB`) is repaired too: the tenant's history is
  written anew from memory, as new segments that replace the old ones.
  ([A store that cannot write its log](doc/installation.md#a-store-that-cannot-write-its-log))
