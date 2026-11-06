### Fixed

- A forwarded write carries only what the registration part it goes to covers (TS 104-175 § 4.3.6.2). Each
  element of a registration's `information[]`, with each element of its `entities[]`, is a part of its own:
  an attribute reaches a source only through a part whose `entities[]` cover the entity - its type when the
  request gives one, its `id`, or its `idPattern` - and whose attributes include the attribute. Before, the
  type of a part was never checked (a Bus created with `a` and `b` sent `a` to a source registered for
  `a` of Cars only), batch upsert, update and merge handed every part's attributes to any entity the
  registration matched (`a`, pinned to `urn:T:A`, went with `urn:T:B`), and batch create took any part with an
  `idPattern` without matching it. A write that does not say the
  entity's type (`PATCH`/`PUT`/`DELETE .../attrs/{attr}`, `POST .../attrs`, a `PATCH` without `type`) takes it
  from the local copy - read only when a registration matched; without a local copy the type is not checked.
