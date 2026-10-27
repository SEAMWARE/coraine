### Fixed

- The DDS bridge starts with a list of plain values in `dds.ddsmodule.dds`, such as
  `"whitelist-interfaces": ["192.168.1.10"]` - the DDS Enabler refused the file and the broker did not start
  (corDdsBridge). A change of the file while the broker runs reloads `allowlist`, `blocklist` and `topics`
  with such a list in it too.
  ([Bridges and Channels](doc/bridge-channels.md))
