### Changed behaviour

- `-fg` / `--foreground` is removed, as announced: a broker started with it, or with `CORAINE_FOREGROUND=true`,
  does not start - it says why on stderr and exits with 1. The broker runs in the foreground by default, and
  `--daemon` detaches it. Through 0.7.x the option did nothing but write a warning line.
