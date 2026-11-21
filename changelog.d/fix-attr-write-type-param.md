### Fixed

- `?type` on the write operations of one Entity selects it: a type that names none of the Entity's types is a
  404 ResourceNotFound, as for an unknown id, and nothing is written. Set Attribute Value, Partial Attribute
  Update, Replace Attribute, Delete Attribute, Update Attributes, Append Attributes, Merge Entity, Replace
  Entity and Delete Entity took the parameter and ignored it - a `DELETE /ngsi-ld/v1/entities/{id}?type=Car`
  deleted a Vehicle.
