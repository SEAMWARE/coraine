### Faster

- Service Execution keeps its pace: with an id index in corDB's document store (corDB#52) and a bounded store,
  58 800 services/s and 42 400 actions/s (with progress) on one core, the loopback executor, TRoE on - it was
  2 840 and 1 600, falling as executions piled up, and every 5 s the broker stalled for up to 0.8 s to sweep
  all of them. ([Performance](doc/performance.md#service-execution))

### Changed behaviour

- Ended Service Executions are kept an hour, not 24, and at most 1000 per tenant - the oldest to end goes
  first; a combined or grouped execution counts as one and goes with its children. `--executionRetention`
  and `--executionMaxEnded` set them. ([Command-line options](doc/options.md#service-execution))

### Fixed

- A child of a combined or grouped execution cancelled because its parent ended (a cancel, a sequential
  child that failed) lost its parent: it was then deleted on its own, and could go before the parent did.
