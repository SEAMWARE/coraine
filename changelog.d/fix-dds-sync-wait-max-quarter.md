### Fixed

- `--ddsSyncWaitMax` defaults to a quarter of the worker threads, at least one - it was a fixed 8, meant
  as a quarter of 32 workers, so on a broker with 8 workers or fewer (corDB on a machine of 8 CPUs or
  fewer) the requests waiting for a slow or absent DDS service could take every worker and stop the
  broker answering. With 32 workers it is 8, as before.
  ([Command-line options](doc/options.md#--ddssyncwaitmax))
