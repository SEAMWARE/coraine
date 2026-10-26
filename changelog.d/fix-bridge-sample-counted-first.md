### Fixed

- A Channel's `samplesIn` (`GET /ngsi-ld/v1/channels`) counts a sample before its value is written, not after
  its notifications and history: a client that saw the new value could read the count still without it.
