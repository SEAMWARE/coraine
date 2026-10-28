### Changed behaviour

- Between releases, `main` reports the last release with `+dev` (`0.6.0+dev`), not the next one with
  `-dev`: the next release's number is decided when it is cut. Its Debian packages are
  `0.6.0+git<date>.<sha>` (after 0.6.0) and its images `0.6.0-post-<date>-<commit>`
  ([Installation](doc/installation.md), [Kubernetes](doc/kubernetes.md)).
