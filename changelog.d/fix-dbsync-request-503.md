### Fixed

- corDB with `--dbDir`: a write whose record cannot be put in the log - a full disk - is answered 503, with
  any `--dbSync`; it was answered as a success, and lost at the next start. With `--dbSync request`, a write
  whose `fdatasync` fails is answered 503 too; it was answered as a success, and a write after an earlier
  failure no longer waited for its sync.
  ([`--dbSync`](doc/options.md#--dbsync))
