### New

- `notification.geometryProperty` in a Subscription: the GeoProperty whose value is the `geometry` of a GeoJSON
  notification (`"accept": "application/geo+json"`), as the `geometryProperty` URL parameter is for a query.
  Without it the geometry is `location`, as before; an entity positioned through another GeoProperty had
  `"geometry": null`. Only with a GeoJSON `accept` - otherwise 400 BadRequestData. Proposed for the spec in
  TS 104 175 issue #67. ([API walkthrough](doc/api-walkthrough.md#5-subscribe))
