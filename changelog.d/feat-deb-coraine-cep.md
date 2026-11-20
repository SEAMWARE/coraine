### New

- A third Debian package, `coraine-cep`: corCep's rules engine as the bridge plugin `cep.so`
  (`coraine --bridges cep`). It is built with the broker of the same version, and `coraine` depends on exactly
  that version, so `apt install coraine` brings it. `coraine-build` builds the CEP bridge by default
  (`--bridges mqtt,modbus,cep`).
  ([Installation](doc/installation.md#install-with-apt))
