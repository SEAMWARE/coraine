### Fixed

- `POST /ngsi-ld/v1/entityOperations/upsert` on a tenant that does not exist yet creates the tenant, as
  Create Entity and Batch Create do - it answered 404 NonexistentTenant and wrote nothing. A query, an
  update, a PATCH or a DELETE on an unknown tenant is still a 404 NonexistentTenant.
  ([Multi-tenancy](doc/installation.md#multi-tenancy))
