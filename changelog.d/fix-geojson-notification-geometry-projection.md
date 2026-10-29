### Fixed

- A GeoJSON notification whose `pick`, `omit` or `attributes` left out the geometry's GeoProperty (`location`
  or the `notification.geometryProperty`) had `"geometry": null`; with a `geometryProperty`, such a
  subscription was refused (400). The geometry is now taken before the projection, as for a query
  (§ 5.3.3.2); the GeoProperty is in `properties` only when the projection keeps it.
