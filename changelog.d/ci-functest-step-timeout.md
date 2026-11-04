### Fixed

- A functional-test shard in CI that hangs is stopped by a limit of its own (25 minutes), under the job's,
  so the job still keeps the test outputs and logs, and records which test was running and what was still
  alive. At the job's limit GitHub ended the job before any of that ran, and the log was lost.
