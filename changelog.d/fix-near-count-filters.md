### Fixed

- `georel=near` with `count=true` counts what the query matches: `NGSILD-Results-Count` had neither the type nor
  `q` applied - every Entity near the point was counted, of any type. `type=Vehicle` near a point answered 15
  where 4 Vehicles were returned and 12 existed in all. The Entities returned were right. mongoc.
