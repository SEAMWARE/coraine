### Fixed

- A ListRelationship follows TS 104-175 § 5.2.6.4.9: an `objectList` item is accepted as a URI or as
  `{"object": <URI>}`, and the normalized output (an entity, an attribute, a temporal evolution, a notification)
  gives every item as `{"object": <URI>}`, `previousObjectList` included. Concise and simplified keep the URIs.
  An `objectList` of `{"object": <URI>}` items was refused with 400.
- `["urn:ngsi-ld:null"]` as a `valueList` or an `objectList` is the NGSI-LD Null: the core `@context` declares both
  `"@container": "@list"`, so it deletes the attribute in an update, and is refused in a create. A notification
  gives a deleted ListProperty or ListRelationship as `["urn:ngsi-ld:null"]`.
- Linked entity retrieval (`join=inline`, `join=flat`, in a query and in a notification) follows a
  ListRelationship's targets: inline adds them as `entityList`, simplified gives the attribute as those entities.
