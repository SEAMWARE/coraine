### Fixed

- The arm64 image builds again - none was published since 2026-10-22: its profile-guided build stopped on
  gcc's fortify check (`null destination pointer`) in the metrics' text, which measured its length into a NULL
  buffer. It measures into a byte thrown away now; the TimescaleDB plugin's statement text the same.
