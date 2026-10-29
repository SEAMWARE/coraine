### Fixed

- The temporal API renders its times as `/entities` does: to the microsecond (to the nanosecond with
  `--high-precision`, for `--troe corDB`), the fraction's trailing zeros left out. An `observedAt` sent as
  `.290` was `.29` in `/entities` and `.290` in `/temporal`; `--troe corDB` gave `observedAt` to the
  millisecond only, `--troe timescale` its `createdAt` and `modifiedAt` with all six digits (`.123000`),
  and rounded where `/entities` truncates - one microsecond apart for every other write.
  ([--high-precision](doc/options.md#--high-precision))
