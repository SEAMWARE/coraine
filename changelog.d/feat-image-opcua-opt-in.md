### New

- The coraine image can carry the OPC UA bridge, `opcua.so` (`--bridges opcua`): built with `OPCUA=on`
  (`make docker OPCUA=on`, `--build-arg OPCUA=on`), and only then - it brings open62541 (the image 12 MB bigger, with
  mbedTLS). The image build loads it once and fails if it does not load. ([docker/README.md](docker/README.md))
