### Fixed

- `DELETE /entities/{id}/attrs/scope` removes the scope again, with a `deletedAt` in the temporal history
  (TS 104-175 § 10.2.7.4: "If the target Attribute is scope, remove the scope Attribute from the target
  Entity"). 0.7.0 refused it with 400 BadRequestData. A term of the request's `@context` for `ngsi-ld:scope`
  works the same. `PATCH`, `PUT` and `PUT .../value` on `.../attrs/scope` still answer 400 (§ 10.2.5.4,
  § 10.2.6.4, § 10.2.11.4), and `DELETE .../attrs/id` and `.../attrs/type` still answer 400.
