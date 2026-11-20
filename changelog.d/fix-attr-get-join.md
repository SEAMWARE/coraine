### New

- Retrieve Attribute (`GET /ngsi-ld/v1/entities/{id}/attrs/{attr}`) takes `join` and `joinLevel` (TS 104 176
  § 7.5.3.1): with `join=inline` a Relationship comes back with its linked entity nested as `entity`, down to
  `joinLevel` (1 by default). `join=@none` leaves it as it is, and `join=flat` is a 400 BadRequestData - an
  attribute has no flat form. Before, both parameters were refused as unknown.
