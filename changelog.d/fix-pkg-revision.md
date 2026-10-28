### Changed behaviour

- The Debian packages can be built again for a release with a fix of c³'s, without a new release: a
  package revision, version `<X>+<dist>.<N>` (`0.6.1+debian13.1`), which `apt upgrade` installs over
  `<X>+<dist>` - the broker the same, c³ at a later commit (named in `MANIFEST.txt`). The `Packages`
  workflow, run by hand, takes `ref`, `c3_ref` and `revision` for it.
  ([Install with apt](doc/installation.md#install-with-apt))

### Fixed

- A build of a commit between releases (`0.6.1+git<date>.<sha>+<dist>`) published to the apt repository
  went into the component `main`, beside the releases, instead of `testing`.
  ([Install with apt](doc/installation.md#install-with-apt))
