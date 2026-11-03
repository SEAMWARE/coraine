### Changed behaviour

- `DELETE /entities/{id}/attrs/scope` answers 400 BadRequestData. Before, it removed the scope (§ 10.2.7.4).
  The scope is changed through the Entity only: Merge Entity, Update Attributes or Append with
  `"scope": "urn:ngsi-ld:null"` deletes it.

### Fixed

- `PATCH`, `PUT`, `PUT .../value` and `DELETE` on `/entities/{id}/attrs/{id,type,scope}` answer 400
  BadRequestData, "'<name>' is an Entity member, not an Attribute". The same holds for a term of the
  request's `@context` that aliases `@id`, `@type` or `ngsi-ld:scope`. Before, PATCH and PUT on id/type gave a
  400 with a misleading detail, and `PUT .../value` gave a 404.
- A sub-attribute named `id` is refused (400 BadRequestData) on every route. Before, it was stored.
- `PATCH /entities/{id}/attrs/{attrId}` stores a concise sub-attribute (`"note": "x"`) as a Property, as
  Merge Entity does. Before, it was stored as a bare string.
