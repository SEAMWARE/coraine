### Fixed

- `GET /assetChannels/{id}` (and `/channels/{id}`, `/dataChannels/{id}`) finds a channel whose id carries a URL -
  `urn:ngsi-ld:AssetChannel:aas:https://acme.example/ids/aas/press-17`, its endpoint an AAS shell's id -
  percent-encoded or as it is. It answered 400 "empty path segment".
