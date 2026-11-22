### Fixed

- Set Attribute Value (`PUT /entities/{entityId}/attrs/{attrId}/value`) on a ListRelationship takes the
  normalized `objectList` form, `[{"object": <URI>}, ...]` (TS 104-175 § 5.2.6.4.9). It was refused with
  400 "'objectList' items must be URIs or {"object": <URI>}" (ETSI test suite 063_01_08).
