### Fixed

- coraine builds again with a compiler whose default C standard is older than C23 (gcc 13, Ubuntu
  24.04's): `src/lib/probe/probe.c` used `false` without `<stdbool.h>`, a keyword only from C23.
