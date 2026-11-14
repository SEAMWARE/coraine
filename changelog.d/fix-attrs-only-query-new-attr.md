### Fixed

- The deprecated `attrs` selects again on every page: an entity with one of the listed attributes was lost when
  it was not among the store's first page (20 by default) - the selection was made after the page was cut.
  `GET /entities?attrs=A` answered `[]` with `A` on the 25th entity, and `count=true` counted every entity
  of the page's query. The selection is now the store's (an existence test per attribute, in OR, with `q`),
  as `q=A`'s is. mongoc, corDB, ramDB.
