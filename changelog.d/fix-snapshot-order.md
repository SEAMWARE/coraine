### Fixed

- On Ubuntu, a Debian package of a commit between releases sorted before the release it follows
  (`0.6.1+git<date>.<sha>+ubuntu24.04` < `0.6.1+ubuntu24.04`), so `apt upgrade` did not install it over the
  release. Its version is now `0.6.1+<dist>.<YYYYMMDD>.git<sha8>`: after the release and its package
  revisions, before the next release, on every distribution.
  ([Install with apt](doc/installation.md#install-with-apt))
