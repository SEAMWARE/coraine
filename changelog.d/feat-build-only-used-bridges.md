### New

- `coraine-build --source <clones>` needs only the bridge clones `--bridges` names: a broker built without dds
  needs no `corDdsBridge` clone, nor `corCkanBridge`, `corOpcuaBridge`, `corAasBridge` or `corCep` when those
  are not built. `docker/vendor-libs.sh` exports the bridge repositories `VENDOR_BRIDGES` lists (unset: all
  seven - the image is unchanged); corLibs' makefile builds the bridge directories that are there.
- `docker/vendor-libs.sh --list` prints the repositories it would export (with `VENDOR_BRIDGES`), and
  `coraine-build --list-bridges --json` names each bridge's `repository` - what c³'s Compile checks a directory
  of clones against before a build.
