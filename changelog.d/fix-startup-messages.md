### Fixed

- The start-up log says a size below 1 MiB in KiB: `--memoryLimit 1` logged "response size budget: 0 MiB" (it is
  64 KiB) and "writes refused above 0 MiB" (921 KiB); likewise the 403 of a response over its budget and the 503
  over the memory budget. `--healthPort` on a port that cannot be listened on says why ("Address already in use"),
  as `--corPort` does.
