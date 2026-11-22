### New

- `coraine-build --bridges aas` builds the AAS bridge (`aas.so`, [corAasBridge](https://github.com/SEAMWARE-cor/corAasBridge) -
  a prototype): an Asset Administration Shell server's shells and submodels as AssetChannels. It needs
  OpenSSL's headers (libssl-dev); `--list-bridges` says so where they are missing. The source tarball carries it.
  ([AAS](doc/aas.md))
