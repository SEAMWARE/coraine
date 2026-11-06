### Fixed

- A query forwarded to a Context Source asks only for what the source's registration covers
  (TS 104-175 § 4.3.6.2). Each element of the registration's `information[]` and each element of its
  `entities[]` is a part of its own: the forward carries the part's types, its ids or `idPattern`, and the
  attributes of that `information[]` element as `pick`. A registration whose parts differ gets one forward
  per part. Before, the client's whole type list went to every source, so a query for
  `type=OffStreetParking,Vehicle` returned the Vehicles of a source registered for OffStreetParking only;
  the attributes of the first matching `information[]` element (or of all of them, with split entities)
  went with every type; client ids outside the registration were passed on; and an auxiliary registration
  was asked for types it does not have. A client `id` list is narrowed to the registration's ids, and to
  the ids its `idPattern` matches; a client `idPattern` is applied to the answer.
