### Fixed

- A trailing `/` after a parameter value is the empty last segment of the longer resource, not dropped:
  `PATCH` and `DELETE /ngsi-ld/v1/temporal/entities/{entityId}/attrs/{attrId}/` are Attribute Instance by id with
  an empty instanceId - 400 BadRequestData. The `PATCH` answered 405, and the `DELETE` deleted the attribute's
  whole history. A `/` after a fixed segment still names the collection: `/entities/`, `/entities/{entityId}/attrs/`.
- An attribute name in the URL is one path segment (or a URL): `GET /ngsi-ld/v1/entities/{entityId}/attrs/a/b` is
  404 - it reached the attribute with the name `a/b`.
