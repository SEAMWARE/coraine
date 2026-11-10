### Changed behaviour

- A HEAD on `/ngsi-ld/v1/entityMaps` or `/ngsi-ld/v1/temporal/entityMaps` creates an EntityMap as the GET
  does, and now answers 201 with the `NGSILD-EntityMap` header and no body. It used to answer 204. Decided in
  the ETSI NGSI-LD API call of 2026-11-10.
