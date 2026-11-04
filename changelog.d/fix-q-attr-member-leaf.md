### Fixed

- q on an attribute's members follows TS 104-175 § 7.2.3: `name.valueType`, `rel.objectType` and
  `temp.unitCode` compare the member itself. Only their existence test used to match.
- `valueType`, `objectType`, `vocab` and `previousVocab` values are expanded as the core `@context` says: a
  core term gets its IRI (`langString` is stored as `http://www.w3.org/1999/02/22-rdf-syntax-ns#langString`),
  and a q value on `valueType` or `objectType` is expanded the same way, so the short form and the IRI are
  one query. Responses still show the short form.
