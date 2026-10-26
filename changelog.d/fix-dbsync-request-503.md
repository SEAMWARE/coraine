### Fixed

- corDB with `--dbDir`: a write whose record cannot be put in the log - a full disk - is answered 503, with
  any `--dbSync`; it was answered as a success, and lost at the next start. After a failed `fdatasync`,
  every later write of the tenant is answered 503 until a restart, with any `--dbSync` - a load stops at the
  first record it cannot read; with `--dbSync request`, the writes that sync was for are answered 503 too.
  They were all answered as successes, and a `--dbSync request` write after an earlier failure no longer
  waited for its sync.
  ([`--dbSync`](doc/options.md#--dbsync))
