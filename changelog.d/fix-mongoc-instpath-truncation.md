### Fixed

- The release and PGO builds (`make release`, `make pgo` - the published images) failed to compile the mongoc
  backend's `[*]` q code (`-Werror=format-truncation`): a buffer one suffix too small. The debug build did not
  warn. The images of #571's merge were not published.
