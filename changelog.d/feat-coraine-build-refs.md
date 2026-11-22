### New

- `coraine-build --ref <repo>=<ref>` (with `--source <dir>`): another branch, tag or commit of one clone goes
  into the build - once per repository, e.g. `--ref coraine=origin/main --ref corNgsild=feat/x`. Checked before
  anything is exported; MANIFEST.txt names each ref; `--list-features` and `--list-bridges` read the refs too.

### Documentation

- doc/options.md: `coraine-build --image` has its entry (it had none; tools/optionsDocumented.sh found it).
