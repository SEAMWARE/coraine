### New

- The coraine image carries the AAS and CKAN bridges, `aas.so` and `ckan.so` (`--bridges aas`, `--bridges ckan`).
  The image build loads both once and fails if one is missing or does not load. The OPC UA bridge is not in
  the image: it links open62541. ([docker/README.md](docker/README.md))
