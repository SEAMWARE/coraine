### Fixed

- A multi-attribute in concise format (`options=concise` - entities, queries, a single attribute,
  notifications) is an array of objects only: the default instance with nothing but its value is
  `{ "value": "Eiffel Tower" }`, no longer the bare `"Eiffel Tower"` beside the other instances' objects - as
  decided in the ETSI test-suite group (MR !304, 018_23_10). A GeoProperty instance is
  `{ "value": <geometry> }`. A bare value inside the array is still accepted on input.
