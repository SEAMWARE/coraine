### Changed behaviour

- A Bool option's environment variable is on for `true`, `True`, `TRUE`, `on`, `On` or `ON`, off for `false`,
  `False`, `FALSE`, `off`, `Off` or `OFF`. Any other value stops the start, the variable named
  (`coraine: CORAINE_VERBOSE=yes: not a Bool - ...`) - it was off without a word, `yes` and `1` included, and
  `on` was off too.
  ([Command-line options](doc/options.md#command-line-options))

### Fixed

- `CORAINE_PORT=0 coraine -u` printed the port's range error, not the usage: a value of the environment that
  is refused is now said after the `-u`/`-U` check, as a bad value on the command line is.
  ([Command-line options](doc/options.md#command-line-options))
