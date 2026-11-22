### New

- An AssetChannel operation's array variable (`"array": true` - an OPC UA method's array argument) is an array
  in its Service Registration's schema, `{ "type": "array", "items": ... }`, and a variable without a
  `valueType` takes any value - the input of an OPC UA method with an array argument is checked as the method
  takes it. ([AssetChannels § 4](doc/asset-channels.md#4-operations-and-methods-service-execution))
