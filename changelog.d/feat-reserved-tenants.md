### New

- `--reservedTenants`: tenants kept apart from the data - corCep's rules tenant is one. Reached with
  `NGSILD-Tenant` as any other tenant, but stored in a database of its own (mongoc `<dbName>_r-<tenant>`,
  corDB `_r-<tenant>`), so the rules never show in an ordinary tenant's queries, `/types` or `/attributes`;
  not listed by `GET /admin/tenants`. Found again at start by its database; a name whose ordinary tenant
  already has data is refused.
  ([Command-line options](doc/options.md#--reservedtenants))
