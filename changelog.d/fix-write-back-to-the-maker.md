### Fixed

- A write to an entity that two write-back DataChannels' `id` templates both fit (two bridges each making
  `urn:ngsi-ld:Light:{@deviceId}`) goes to the DataChannel whose record made the entity - it went to the
  first that fits, whose bridge has no such device: 400.
  ([DataChannels](doc/bridge-channels.md#datachannels))
