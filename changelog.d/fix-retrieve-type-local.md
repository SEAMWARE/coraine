### Fixed

- `GET /ngsi-ld/v1/entities/{id}`, `.../attrs/{attrId}` and `.../attrs/{attrId}/value` honour `?type` on the local
  entity: a type that is none of the entity's types answers 404 ResourceNotFound (TS 104-175 § 10.4.2.4,
  § 10.4.4.4). `?type` only scoped the registrations a retrieve is forwarded to, and the local entity came back
  whatever its type.
