### Fixed

- `corDB`: `--dbSync`, `--dbSyncInterval`, `--dbSnapshotEvery` and `--dbCompress` without `--dbDir` were not
  looked at - `--dbSync sometimes` started, and `--dbSync request` promised a durability there was none of.
  Given without `--dbDir` they now refuse the start, each one named, as `ramDB` refuses `--dbDir`.
  ([corDB on disk](doc/installation.md#cordb-on-disk))
