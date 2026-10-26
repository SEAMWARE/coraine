### Changed behaviour

- No `--debug` / `-d` (`CORAINE_DEBUG`), for the broker and `coraine-import`: it turned on the `D` lines of
  the log, and nothing wrote one. A start with `-d` is refused as an unknown option. What shows more is the
  trace levels (`--traceLevels`). `/admin/log` has no `debug` either: `GET` no longer reports it, and
  `?debug=` is refused (400).
  ([Command-line options](doc/options.md#--tracelevels))
