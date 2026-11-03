### Changed behaviour

- The CI performance job (nightly and pre-release campaign) warns instead of failing: a slowdown against
  the recorded history is a warning annotation plus the job summary, and the runner's CPU model is printed.
  A flagged slowdown is measured again locally on one fixed machine, and that run decides
  (doc/testing.md, "Performance in CI").
