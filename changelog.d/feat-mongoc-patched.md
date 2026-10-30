### Faster

- The image, the CI and the `.deb` packages that bundle the MongoDB C driver (Ubuntu 24.04, Debian 13) build
  2.5.7 with four patches of the stack's: a command's retry state made only when it is retried, retryReads /
  retryWrites read once per client, a reply's error fields found in one pass, an implicit session without
  transaction state. The broker runs 9.5 % fewer instructions per `PATCH` and 7.4 % per retrieve (mongoc);
  requests/s are within their runs' spread. The patches, and the driver's licence, ship with it
  (`/usr/local/share/doc/mongo-c-driver` in the image, `/usr/share/doc/coraine/mongo-c-driver` in a package).
  Ubuntu 26.04's driver is the distribution's.
  ([Performance](doc/performance.md#the-stacks-patches-to-mongo-c-driver-257))

### Fixed

- The image ships the MongoDB C driver's licence and third-party notices with the library (it had neither).
