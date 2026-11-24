### New

- `COR_FEATURE_CORDB` (`make release COR_WITH_CORDB=0`, `coraine-build --features CORDB=OFF`): a broker without
  corDB - the corDB repository is not built (`corDB.so`, `ramDB.so`, the TRoE `ramDB.so`), `TROE_CORDB` is off
  with it (`--list-features --json`: `"requires": ["CORDB"]`). A build with neither `MONGOC` nor `CORDB` is
  refused. `--pgo` / `--tune` refuse `CORDB=OFF` (their training runs corDB).
- `--database` defaults to `corDB` in a broker built without mongoc (`MONGOC=OFF`); `coraine-import` the same.

### Fixed

- A broker built without a store (`MONGOC=OFF`, `CORDB=OFF`) refuses it by name - `--database mongoc`,
  `corDB`, `ramDB`, `--troe ramDB` - and leaves it out of its usage: a `.so` left in the plugin directory by
  another build was loaded and run.
- `make install` of a build with `MONGOC`, `TROE_TIMESCALE`, `ADMIN_API` or `TRANSPORTS` off failed on the
  plugin it did not build.
