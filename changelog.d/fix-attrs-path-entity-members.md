### Fixed

- `PATCH`, `PUT` and `PUT .../value` on `/entities/{id}/attrs/id` or `.../attrs/type` (or a term of the
  request's @context that aliases `@id`/`@type`) answer 400 BadRequestData: id and type are Entity members,
  not Attributes. Before, PATCH and PUT gave a 400 with a misleading detail, and `PUT .../value` gave a 404.
- A sub-attribute named `id` is refused (400 BadRequestData) on every route. Before, it was stored.
- `PATCH /entities/{id}/attrs/{attrId}` stores a concise sub-attribute (`"note": "x"`) as a Property, as
  Merge Entity does. Before, it was stored as a bare string.
