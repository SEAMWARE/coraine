### Changed behaviour
- A retrieve by id - `GET /entities/{id}` and `GET /temporal/entities/{id}` - answers 200 for an existing entity
  whatever `attrs`, `datasetId` and the time range leave: `attrs` is a projection, the Entity ID selects the entity
  (ts-104-175!42, ts-104-176!20). The entity with none of the attributes: `{ id, type }`; on the temporal API also
  each requested attribute the entity has, without instances - `[]`, or `{ "type", "values": [] }` (temporalValues),
  `{ "type", "<method>": [] }` (aggregatedValues). Was 404 ResourceNotFound. An entity the store (or, on the temporal
  API, the temporal database) does not have is still 404.
