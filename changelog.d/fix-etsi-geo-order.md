### New

- Ordering by distance from any reference geometry: `orderGeometry=LineString`, `Polygon`, `MultiPoint`,
  `MultiLineString`, `MultiPolygon` beside `Point` (TS 104-175 § 7.6.2). An entity's `geoDistance` is the
  shortest distance between the reference and its GeoProperty - 0 when they meet.

### Fixed

- A Query Entities whose `orderFrom` held something that is not a number (`orderFrom=[13.4,invalid]`) never
  answered, on MongoDB: the broker parsed the coordinates forever. `orderFrom` is now checked as the
  coordinates of a geo-query are: not JSON, or not the coordinates of the `orderGeometry` (a single number for
  a Point, a position for a LineString) is 400 BadRequestData - on MongoDB it was a 500, on corDB a 200.
- A distance order beside a geo-query: with `georel=within` (and every georel but `near`) the order was
  ignored, and with `georel=near` the entities were ranked by their distance from the georel's point, not from
  `orderFrom`.
- corDB: `georel=near` and a distance order measured only Point GeoProperties - an entity whose GeoProperty
  is a LineString or a Polygon had no distance; it now has the distance to the nearest point of its geometry,
  as on MongoDB. A distance between two geometries that are not both Points is the haversine distance between
  their nearest points (it was the planar distance in degrees times 111320 m, which at 52° N made an east-west
  distance 60 % too long) - this also serves `near` against a registration's geometry.
