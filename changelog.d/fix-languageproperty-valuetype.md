### Fixed

- A LanguageProperty whose `valueType` is not `langString` (or its IRI, `rdf:langString`) is rejected with 400
  BadRequestData on create and update (TS 104-175 § 5.2.6.4.6) - any string was accepted.
