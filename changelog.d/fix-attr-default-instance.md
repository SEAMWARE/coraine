### Fixed

- `GET /ngsi-ld/v1/entities/{entityId}/attrs/{attrId}` and `.../attrs/{attrId}/value` without `datasetId` return
  the default instance only (TS 104-175 § 10.4.4), and 404 when the attribute has no default instance - they
  returned every instance.
