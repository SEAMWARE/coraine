### Fixed

- The image, the CI and the `.deb` packages that bundle it (Ubuntu 24.04, Debian 13) are built with the MongoDB C
  driver 2.5.7 (was 2.2.2) - the fixes of its 2.3 to 2.5 releases, about 14 CVEs among them (JSON and BSON
  parsing, SCRAM and SASL, OCSP, name validation). Ubuntu 26.04's comes from the distribution, 2.2.2. 2.5 is the
  version MongoDB recommends for server 9.0.
