### New

- An AssetChannel's operations are services: each operation of the model (an AAS Operation) becomes a Service
  Registration of the entity it is in - `serviceName` its name, its input and output variables the
  `inputSchema` and `outputSchema` (an `xs:*` type a JSON type, a bounded integer its range), the endpoint the
  bridge gives - so `POST /ngsi-ld/v1/entities/{id}/services/{name}` invokes it. A new model creates, replaces
  and deletes them as its operations come, change and go; an AssetChannel configured away loses them; a
  client's own registration is never touched. `COR_FEATURE_ASSET_CHANNEL_SERVICES`.
  ([AssetChannels § 4](doc/asset-channels.md#4-operations-and-methods-service-execution))
