### Fixed

- `GET /ngsi-ld/v1/entities/{id}`, `.../attrs/{attrId}` and `.../attrs/{attrId}/value` honour `?type` on the local
  entity: a type that is none of the entity's types answers 404 ResourceNotFound (TS 104-175 § 10.4.2.4,
  § 10.4.4.4). `?type` only scoped the registrations a retrieve is forwarded to, and the local entity came back
  whatever its type.
- `GET /ngsi-ld/v1/entities/{id}?type=...` with a matching registration: a local copy of another type is no
  longer merged into the sources' Entity, and the assembled Entity must have a requested type (an auxiliary
  registration is matched by id only) - else 404. `?type=Vehicle` could answer a merged `Building`.
