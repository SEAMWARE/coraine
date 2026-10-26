### Fixed

- A required option missing from the command line is named, the usage printed, exit 1: `coraine-import` without
  `--file` crashed (SIGSEGV; with `mongoc` after connecting). `-u` / `-U` print the option list whatever else the
  command line holds - `coraine -u -p 0` exited 1 with the range error of `--port`. The action options (`--usage`,
  `--Usage`, `--version`) have no environment variable: `-U` listed `CORAINE_VERSION`, which was never read.
