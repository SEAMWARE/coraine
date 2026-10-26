### Fixed

- The profile-guided build stops no more on gcc's fortify check (`null destination pointer`) in the metrics'
  text, which measured its length into a NULL buffer: the arm64 image is published again (none since
  2026-10-22), and the nightly's image builds (performance, the DDS bridge, the fdsc plugin vs the Data
  Space Connector) build again. It measures into a byte thrown away now; the TimescaleDB plugin's statement
  text the same.
