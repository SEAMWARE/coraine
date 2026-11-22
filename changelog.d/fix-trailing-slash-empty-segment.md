### Fixed

- An attribute name in the URL is one path segment (or a URL): `GET /ngsi-ld/v1/entities/{entityId}/attrs/a/b` is
  404 - it reached the attribute with the name `a/b`. A 404 for a path ending in `/attrs/{attrId}/value` names the
  whole path - it lost the `/value`.
