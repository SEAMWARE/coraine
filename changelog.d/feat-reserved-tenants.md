### New

- `--reservedTenants`: tenants kept apart from the data - corCep's rules tenant is one. Reached with
  `NGSILD-Tenant` as any other tenant, but stored in a database of its own (mongoc `<dbName>_r-<tenant>`,
  corDB `_r-<tenant>`), so the rules never show in an ordinary tenant's queries, `/types` or `/attributes`;
  not listed by `GET /admin/tenants`. Found again at start by its database. A name whose ordinary tenant
  already has data stays ordinary, with a warning - ordinary data always wins, and the broker always starts.
  ([Command-line options](doc/options.md#--reservedtenants))
- Plugins can reserve a tenant themselves (`tenantReserve`) and visit the data tenants - the default one and
  every ordinary tenant, the reserved ones left out (`tenantDataVisit`): corCep's `cep.so` runs its rules in
  each of them.
