### Changed behaviour

- A q term with `[*]` (no natural language specified) matches a LanguageProperty only, as decided in the ETSI
  NGSI-LD API call of 2026-11-10. On a Property whose value is a JSON object, `description[*]=="pain"` used to
  match through the object's keys. Now `[*]` there does not resolve: `!description[*]` matches, and every other
  term does not. This holds for the entity query on mongoc and corDB, and for subscriptions.

### Fixed

- A `[*]` query on mongoc answered 500 when an Entity of the queried type had no such Attribute
  (`$anyElementTrue's argument must be an array, but is null`).
