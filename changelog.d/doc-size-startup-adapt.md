### Faster

- With the built-in HTTP server, coraine answers its first request 3.4-4.5 ms after it is started, from
  6.3-8.0 ms: the connection pool's slots are set up on first use, and the port opens only once the server
  can answer. Throughput is unchanged.
  ([Start-up](doc/performance.md#start-up))

### Less memory

- With the built-in HTTP server, an idle coraine is 8.0 MiB resident, from 16.1 MiB: a connection slot
  becomes resident when a connection first uses it. Memory follows the most connections ever open at once.
  ([RAM](doc/performance.md#ram))
