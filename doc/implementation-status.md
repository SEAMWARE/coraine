# swBroker Implementation Status

Version: post-0.2.0
Date: 2026-04-20

---

## Architecture

swBroker is a lightweight NGSI-LD Context Broker built in C on top of:
- **k-libs** (kalloc, kjson, ktrace, kargs, kbase, khash, klog, kprom)
- **sw-libs** (swRest, swJsonld, swNgsild, swPlugin)

Database and API functionality are loaded as **plugins** (`/opt/seamware/plugins`):
- **DB plugins**: `swRamDB` (in-memory, GEOS geo-filtering, per-tenant isolation), `mongoc` (MongoDB via libmongoc, $geoNear aggregation)
- **API plugins**: `admin` (health, version, log, tenants, plugins)

---

## Implemented Endpoints

### 1. POST /ngsi-ld/v1/entities — Create Entity

**Status: Complete, incl. distributed ops**

| Feature | Status |
|---------|--------|
| Normalized format input | Done |
| Concise format input | Done |
| Simplified format input | Done |
| Mixed format input (attrs in different formats) | Done |
| @context in payload (application/ld+json) | Done |
| @context in Link header (application/json) | Done |
| All 8 attribute types (Property, Relationship, GeoProperty, LanguageProperty, VocabProperty, ListProperty, ListRelationship, JsonProperty) | Done |
| Sub-attributes (nested) | Done |
| Multi-attribute (datasetId) | Done |
| System attributes (createdAt, modifiedAt) | Done |
| Multi-tenancy (NGSILD-Tenant header) | Done |
| 201 Created + Location + Link headers | Done |
| 409 AlreadyExists | Done |
| Error handling (missing id, missing type, bad payload, bad Content-Type) | Done |
| scope (entity scope property) | Done |
| Multi-entity-type (type as array) | Done |
| Subscriptions / notifications on create | Done |
| `?local=true` bypass of dispatch (§ 5.5.13) | Done |
| DistOps — exclusive mode forwarding + chop (§ 4.3.6.3) | Done |
| DistOps — redirect mode forwarding + chop | Done |
| DistOps — inclusive mode forwarding + local keep | Done |
| Per-RegistrationInfo dispatch (chop per info entry) | Done |
| CSR `expiresAt` runtime enforcement | Done |
| CSR `tenant` rewrite (§ 5.2.9, NGSILD-Tenant on forward) | Done |
| CSR `contextSourceInfo` outbound headers + § 4.3.6.6 special-cases (accept/contentType) | Done |
| CSR `scope` vs entity scope match | Done |
| CSR `management.timeout` applied to forward | Done |
| CSR `location` / `observationSpace` / `operationSpace` geo match (shared GEOS via `db.geoMatchFunc`) | Done (entity-within-CSR via `LdGeoWithin`) |
| CSR dispatch counters (timesSent/Failed, lastSuccess/Failure) | Done |
| Via loop detection — tenant-scoped alias | Done |
| 207 Multi-Status with `BatchOperationResult` body (§ 5.2.17 / § 6.4.3.1) | Done |
| 409 Conflict with `BatchOperationResult` on complete dispatch failure | Done |
| Exclusive CSR without createEntity op → 207/409 with BatchEntityError (§ 5.6.1.4) | Done |
| `jsonldContext` / `ngsildConformance` contextSourceInfo keys | TODO |
| `urn:ngsi-ld:request` sentinel value (§ 4.3.6.5) | TODO |

Functional tests: `create_entity*.test` (9 files), `tenant.test`, `tenant_persistence.test`, `csource-reg-distops-create-*.test` (9 distops files).

### 2. GET /ngsi-ld/v1/entities — Query Entities

**Status: Mostly complete (local queries, no ordering/join/entityMap)**

| Feature | Status |
|---------|--------|
| Filter by id (comma-separated) | Done |
| Filter by idPattern (regex) | Done |
| Filter by type (simple and compound: `,` OR, `;` AND) | Done |
| q filter (query language) | Done |
| scopeQ filter | Done |
| Geo-query (georel, geometry, coordinates, geoproperty) | Done |
| pick (attribute projection — include only) | Done |
| omit (attribute projection — exclude) | Done |
| datasetId filter | Done |
| lang (LanguageProperty reduction) | Done |
| Pagination (limit, offset, count) | Done |
| Pagination Link headers (next/prev) | Done |
| NGSILD-Results-Count header | Done |
| Output formats: normalized, concise, simplified | Done |
| System attributes (sysAttrs) | Done |
| Multi-tenancy | Done |
| Error handling (incomplete geo params, limit=0 without count, pick+omit mutual exclusion) | Done |
| GeoJSON response (Accept: application/geo+json) | Done |
| geometryProperty (selects GeoProperty for GeoJSON geometry) | Done |
| expandValues (expand q-filter values via @context) | Done (parsing + q-parser hook) |
| jsonKeys (opaque values in q-filter) | Done (parsing, no-op — no type coercion yet) |
| attrs (deprecated) | Not done |
| orderBy (multi-key, asc/desc, missing-sorts-last) | Done |
| collation (BCP47 locale-aware string sort via strcoll_l) | Done |
| orderFrom, orderGeometry (geo-distance sort) | Not done |
| join, joinLevel, containedBy | Not done |
| entityMap (creation + CRUD + frozen pagination) | Done |
| splitEntities (no-split + split mode, post-assembly filters) | Done |
| entityMapLifetime | Not done |
| local | Done |
| csf | Not done |
| Distributed operations | Done (no-split + split, all 4 modes, per-RegistrationInfo, EntityMap pagination) |

Functional tests: `query_entities_*.test` (22 test files incl. distops + orderBy), `tenant_geo.test`, `geojson_response.test`, `url_param_expand_values.test`

### 3. GET /ngsi-ld/v1/entities/{entityId} — Retrieve Entity

**Status: Mostly complete (no join/entityMap, no type disambiguation)**

| Feature | Status |
|---------|--------|
| Retrieve by entity ID | Done |
| Output formats: normalized, concise, simplified | Done |
| System attributes (sysAttrs) | Done |
| pick / omit | Done |
| datasetId filter | Done |
| lang | Done |
| GeoJSON response (Accept: application/geo+json) | Done |
| geometryProperty | Done |
| Multi-tenancy | Done |
| 404 Not Found | Done |
| type (entity type selection for disambiguation) | Done (comma-separated list) |
| join, joinLevel, containedBy | Not done |
| entityMap, entityMapLifetime | Not done |
| Distributed operations | Done (all 4 modes, per-RegistrationInfo dispatch, reg-constrained pick) |

Functional tests: `retrieve_entity_*.test` (5 test files), `geojson_response.test`,
`csource_reg_distops_*.test` (5 test files)

### 4. DELETE /ngsi-ld/v1/entities/{entityId} — Delete Entity

**Status: Complete**

| Feature | Status |
|---------|--------|
| Delete by entity ID | Done |
| 204 No Content on success | Done |
| 404 Not Found | Done |
| Multi-tenancy | Done |
| type (entity type selection for disambiguation) | Done |
| Subscriptions / notifications on delete | Done |
| `local=true` URL param — bypass distops dispatch | Done |
| DistOps — exclusive/redirect/inclusive forwarding | Done |
| op-check on `deleteEntity` (exclusive/redirect → 409 if unsupported; inclusive silent-skip) | Done |
| 207 Multi-Status on partial forward failure (`BatchOperationResult`) | Done |
| Upstream 404 tolerated (silent-skip; local DB_NOT_FOUND also tolerated) | Done |
| Via loop detect → skip forwards but keep local delete | Done |

Functional tests: `delete_entity.test`, `csource-reg-distops-delete-{exclusive,misc,errors}.test`

### 5. PATCH /ngsi-ld/v1/entities/{entityId} — Merge Entity

**Status: Complete**

Implements Merge Entity per § 5.6.17 using the merge-patch procedure of § 5.5.12
(RFC 7396 adaptation with `"urn:ngsi-ld:null"` as the delete marker, since
NGSI-LD forbids bare JSON null). Surgical semantics throughout: only what the
fragment names is touched. swRamDB mutates the live stored tree in place.
mongoc fetches the target, applies the merge in memory, and writes only the
changed attributes as `$set` / `$unset` ops — a PATCH on one attribute of a
2000-attribute entity writes only that attribute plus the entity-level
modifiedAt (read is unavoidable because the merge needs target's existing
attribute types and observedAt presence for correct behavior).

| Feature | Status |
|---------|--------|
| Deep RFC 7396 merge into nested attribute values | Done |
| Add new attribute | Done |
| Delete attribute via `urn:ngsi-ld:null` | Done |
| Delete sub-attribute via `urn:ngsi-ld:null` at any depth | Done |
| Multi-datasetId: target default vs named instance | Done |
| Type union (add new types to the type array) | Done |
| Scope replacement | Done |
| Cascading modifiedAt (entity → attribute → sub-attribute → ...) | Done |
| createdAt preserved | Done |
| Simplified fragment form (e.g. `"color": "red"`), target type preserved | Done |
| `observedAt` URL param — injected into touched instances that previously had one | Done |
| `lang` URL param — scalar value updates only `languageMap[<lang>]` on LanguageProperty | Done |
| `format=simplified` URL param — accepted (merge already type-aware) | Done |
| Attribute type change prohibited (§ 5.6.4.4) — BadRequest 400 | Done |
| 404 Not Found | Done |
| 400 Bad Request (no payload, bad JSON) | Done |
| Multi-tenancy | Done |
| mongoc surgical `$set`/`$unset` path (skips writing untouched attrs) | Done |
| Per-change report for subscription matching (added / modified / deleted + preValue) | Done (produced, not yet consumed) |
| `type` URL param (distops disambiguation) | Done |
| `local=true` URL param — bypass distops dispatch | Done |
| DistOps — exclusive/redirect chop + forward via PATCH | Done |
| DistOps — inclusive clone + forward via PATCH | Done |
| Per-RegistrationInfo slice via `ldEntityFragmentForInfo` | Done |
| `urn:ngsi-ld:null` delete-marker carried verbatim in forwarded body | Done |
| op-check on `mergeEntity` (redirectionOps group) | Done |
| 409 Conflict when CSR claims attr but refuses mergeEntity | Done |
| 207 Multi-Status on partial forward failure (`BatchOperationResult`) | Done |
| Upstream 404 tolerated (silent-skip; local DB_NOT_FOUND also tolerated) | Done |
| Via loop detect → skip forwards but keep local merge | Done |
| Subscriptions / notifications on merge | Done |

Functional tests: `patch_entity.test`, `patch_entity_datasetid.test`,
`patch_entity_url_params.test`, `patch_entity_type_change.test`,
`csource-reg-distops-patch-{exclusive,misc,errors}.test` (3 distops files)

### 6. PUT /ngsi-ld/v1/entities/{entityId} — Replace Entity

**Status: Complete**

Implements Replace Entity per § 5.6.18 using the procedure of § 5.5.12.
Atomic replace at the driver level: swRamDB detaches the old tree and
grafts a clone of the new one in place; mongoc uses
`mongoc_collection_find_and_modify_with_opts` so the find/replace/return-old
is a single server-side operation. The service routine pre-retrieves the
existing entity to enforce the "type shall not change" guard before the
replace — multi-type entities are compared as sets.

| Feature | Status |
|---------|--------|
| Full-entity replacement (attrs not in body are removed) | Done |
| Atomic driver primitive (`entityReplace`) | Done |
| Type-change guard (scalar and multi-type) | Done |
| Body id / URL id consistency check | Done |
| Content-Type validation (415 on non-JSON) | Done |
| 204 No Content on success | Done |
| 404 Not Found | Done |
| 400 on missing id / missing type / bad payload / id mismatch / type change | Done |
| Multi-tenancy | Done |
| Full shared-validator coverage (duplicate keys, bad URIs, conflicting value keys, GeoProperty errors, multi-attr datasetId, observedAt, unitCode, ...) | Done |
| Subscriptions / notifications on replace | Done (deferred notify, LdNotifyEntityUpdate) |
| `type` URL param (distops disambiguation) | Done |
| `local=true` URL param — bypass distops dispatch | Done |
| DistOps — exclusive/redirect chop + forward via PUT | Done |
| DistOps — inclusive clone + forward via PUT | Done |
| Per-RegistrationInfo slice via `ldEntityFragmentForInfo` | Done |
| op-check on `replaceEntity` (updateOps + redirectionOps groups) | Done |
| 409 Conflict when CSR claims attr but refuses replaceEntity | Done |
| 207 Multi-Status on partial forward failure (`BatchOperationResult`) | Done |
| Upstream 404 tolerated (silent-skip; local DB_NOT_FOUND also tolerated) | Done |
| Via loop detect → skip forwards but keep local replace | Done |

Functional tests: `entity_replace.test` (8 cases — happy path + PUT-specific
errors), `entity_replace_errors.test` (17 cases — one per validator class),
`csource-reg-distops-replace-{exclusive,misc,errors}.test` (3 distops files).

### 7. Subscriptions (POST / GET / PATCH / DELETE) + notifications

**Status: Done.** CRUD, in-memory matcher (q, geoQ, scopeQ, entity selector
with id/idPattern/type), throttling, expiration, status recomputation,
notification format (normalized/concise/simplified), counters, and mongoc
persistence/retrieval. datasetId instance filtering in notifications
(§ 5.8.6). Periodic notifications (timeInterval) via separate pernot
cache + background loop thread — can't PATCH between periodic and
normal mode. 80+ dedicated functests.

### 8. JSON-LD Context Hosting (§ 5.13)

**Status: Done.** All four endpoints, all three context kinds, persistence,
and concurrent-download dedup.

| Endpoint | Spec | What works |
|----------|------|------------|
| GET /jsonldContexts | 5.13.5 | List with `details`, `kind` filter, `limit`/`offset`/`count` headers |
| GET /jsonldContexts/{id} | 5.13.4 | Returns the raw JSON-LD body, `Content-Type: application/ld+json` |
| POST /jsonldContexts | 5.13.2 | Body is `{"@context": ...}` (Hosted, object or array) or `{"url": ...}` (Cached). Implicit→Cached upgrade per § 5.13.2.5 |
| DELETE /jsonldContexts/{id} | 5.13.3 | Plain delete or `?reload=true` (Cached only — re-downloads, refreshes the stored body, rolls back to old entry on download failure) |

| Aspect | Notes |
|--------|-------|
| Persistence (mongoc) | Hosted + Cached survive restart; reserved DB `swBroker` (rejected as a tenant `-dbName`); reload-on-startup repopulates the cache. Implicit is intentionally not persisted |
| Persistence (ramdb) | None — by design |
| Concurrent-download dedup | `swldCacheDownloadingAdd/Remove/Check` serialises a single download per URL; concurrent waiters poll the cache (3s timeout) |
| Hosted id | Broker-minted `urn:ngsi-ld:Context:<counter>-<epochSeconds>` |
| Hosted body shapes | Object form fully supported; array form via `swldContextFromTree` (each URL element is downloaded as Implicit and side-cached, inline objects too) |
| Errors | 415 wrong Content-Type, 400 missing/invalid body, 400 reload-on-non-Cached, 404 unknown id, 503 download failure (`LdContextNotAvailable`), 501 plugin without persistence (Phase B fallback) |
| Concurrency model | `pthread_mutex_t` on the cache (lookup/insert/remove/snapshot). DB ops use the standard mongoc client pool — no shared collection handle, so no second-tier sem needed |

Functional tests: `jsonld_contexts.test`, `jsonld_contexts_crud.test`,
`jsonld_contexts_post_array.test`, `jsonld_contexts_reload.test`,
`jsonld_contexts_mongoc_persist.test` (REQUIRE_DB:mongoc).

Known residuals (non-blocking):
- LRU eviction can drop a Hosted/Cached entry before next request; lazy
  reload-from-DB-on-cache-miss would close that.
- Download-failure waiters poll until 3s timeout instead of being
  signalled on failure.
- No OPTIONS/HEAD/CORS coverage tests on the new routes.

### 9. Admin API (plugin)

**Status: Complete**

- GET /admin/health
- GET /admin/version
- GET/PUT/POST/PATCH/DELETE /admin/log (verbose, debug, info, traceLevels)
- GET /admin/tenants
- GET /admin/plugins

### 10. Other

- CORS support (--corsOrigin, --corsMaxAge)
- HEAD requests (auto-generated from GET handlers)
- OPTIONS requests (Allow header with registered verbs)
- GeoJSON response format (application/geo+json → Feature/FeatureCollection)
- 404 for unknown paths
- Usage/help (--usage)

---

## Not Yet Implemented

### Entity Operations

| Endpoint | Spec Section | Complexity | Estimate |
|----------|-------------|------------|----------|
| POST /entities/{entityId}/attrs | 5.5.7 | Medium | 2 days |
| PATCH /entities/{entityId}/attrs | 5.5.8 | Medium | 2 days |
| PATCH /entities/{entityId}/attrs/{attrId} | 5.5.9 | Medium | 1-2 days |
| DELETE /entities/{entityId}/attrs/{attrId} | 5.5.10 | Low | 1 day |
| PUT /entities/{entityId}/attrs/{attrId} | 5.5.11 | Medium | 1-2 days |

### Batch Operations

| Endpoint | Spec Section | Complexity | Estimate |
|----------|-------------|------------|----------|
| POST /entityOperations/create | 5.6.7 | Low | 1 day |
| POST /entityOperations/upsert | 5.6.8 | Medium | 2 days |
| POST /entityOperations/update | 5.6.9 | Medium | 2 days |
| POST /entityOperations/delete | 5.6.10 | Low | 1 day |
| POST /entityOperations/query | 5.7.3 | Low | 1 day |
| POST /entityOperations/merge | 5.6.20 | Medium | 2 days |

### Subscription Operations

**All CRUD + notification engine done** (see section 7 above). MQTT
notification transport and distributed-subscription forwarding remain.

| Missing piece | Spec Section | Complexity | Estimate |
|---------------|-------------|------------|----------|
| MQTT notification transport | 5.10 | Medium | 2-3 days |
| Distributed subscription forwarding | 5.11 | High | 3-5 days |

### Registration / Distributed Operations

**CRUD: Done.** All five CSR endpoints implemented with creation-time
conflict checks (§ 5.9.2), per-tenant cache, mongoc persistence.

**DistOps for retrieveEntity: Done.** Exclusive + inclusive forwarding,
Via header + tenant-scoped loop detection, multi-source merge per
§ 4.5.5.3 (drop expired → newest observedAt → newest modifiedAt).
HTTP forwarding plugin architecture (extensible to binary transport).

| Remaining | Spec Section | Complexity | Estimate |
|-----------|-------------|------------|----------|
| DistOps for queryEntities | 5.7.3 | High | 5-7 days |
| DistOps for write operations (create/patch/delete) | 5.6.x | High | 5-7 days |
| Discovery filter (§ 5.10.2 query params) | 5.10.2 | Medium | 2-3 days |
| Redirect mode forwarding | 4.3.6 | Medium | 2-3 days |
| Auxiliary mode forwarding | 4.3.6 | Low | 1-2 days |

### Type / Attribute Discovery

| Endpoint | Spec Section | Complexity | Estimate |
|----------|-------------|------------|----------|
| GET /types | 5.7.5 | Medium | 2 days |
| GET /types/{type} | 5.7.6 | Medium | 1-2 days |
| GET /attributes | 5.7.7 | Medium | 2 days |
| GET /attributes/{attrId} | 5.7.8 | Medium | 1-2 days |

### Temporal Operations

| Endpoint | Spec Section | Complexity | Estimate |
|----------|-------------|------------|----------|
| POST /temporal/entities | 5.6.12 | High | 3-5 days |
| GET /temporal/entities | 5.7.4 | High | 5-7 days |
| GET /temporal/entities/{id} | 5.7.4 | High | 3-5 days |
| Temporal CRUD (attrs, instances) | 5.6.13-18 | High | 5-7 days |
| Aggregation (aggrMethods, aggrPeriodDuration) | 5.7.4 | High | 3-5 days |

### Missing Query Features (for existing endpoints)

| Feature | Complexity | Estimate |
|---------|------------|----------|
| orderBy / collation | Medium | 2-3 days |
| join / joinLevel / containedBy | High | 5-7 days |
| entityMap / entityMapLifetime | Medium | 2-3 days |
| local flag | Low | 0.5 days |

### Snapshot Functionality (5.16)

Snapshots allow "freezing" query results so consumers can paginate through
a consistent dataset without results drifting between pages. New in v1.9.1.

| Endpoint | Spec Section | Complexity | Estimate |
|----------|-------------|------------|----------|
| POST /snapshots (Create Snapshot) | 5.16.1 | High | 3-5 days |
| POST /snapshots/{id}/clone (Clone Snapshot) | 5.16.2 | Medium | 2 days |
| GET /snapshots/{id} (Retrieve Status) | 5.16.3 | Low | 0.5 days |
| PATCH /snapshots/{id} (Update Status) | 5.16.4 | Medium | 1-2 days |
| DELETE /snapshots/{id} (Delete Snapshot) | 5.16.5 | Low | 0.5 days |
| Snapshot status notifications | 5.16.6 | Medium | 2 days |
| POST /snapshots/purge (Purge Snapshots) | 5.16.7 | Low | 1 day |
| Snapshot storage + query-on-snapshot engine | — | High | 5-7 days |

The heavy part is the background execution engine: Create Snapshot runs the
`snapshotQueries` and `snapshotTemporalQueries` asynchronously, aggregating
all paginated results, storing them locally, and updating `snapshotStatus`
(preparation → success/partial/empty/failure). The broker then serves
standard NGSI-LD queries against the frozen snapshot data.

### @context Hosting

(Done — moved up to the implemented sections.)

---

## DB Driver Interface

Currently supports:

Entity operations:
- `entityCreate` — POST /entities
- `entityRetrieve` — GET /entities/{id}
- `entityQuery` — GET /entities
- `entityDelete` — DELETE /entities/{id}
- `entityMerge` — PATCH /entities/{id}; reuses `ldEntityMerge` in
  swNgsild, fronted in each driver by a `*EntityMergeOne` helper so Batch
  Merge (§ 5.6.20) can share the core
- `entityReplace` — PUT /entities/{id}; atomic via in-place swap (ramdb)
  or `find_and_modify` (mongoc); returns the pre-image for notifications

Subscription operations:
- `subscriptionCreate`, `subscriptionRetrieve`, `subscriptionQuery`,
  `subscriptionUpdate`, `subscriptionDelete`, `subscriptionList`

Plus: `tenantSetup`, `geoMatchFunc`, `versionInfo`, `init`, `close`.

New endpoints will require extending the `DbDriver` interface with:
- `entityAppendAttrs` (POST attrs)
- `entityUpdateAttrs` (PATCH attrs)
- `attrUpdate` (PATCH attr)
- `attrDelete` (DELETE attr)
- `attrReplace` (PUT attr)
- Registration CRUD
- Temporal CRUD (if supported)

---

## Summary

| Category | Endpoints | Done | Remaining |
|----------|-----------|------|-----------|
| Entity CRUD | 8 | 6 | 2 |
| Batch Operations | 6 | 0 | 6 |
| Subscriptions | 5 + engine | 5 + engine | MQTT, distops |
| Registrations | 5 + distops | 5 + retrieve distops | query/write distops |
| Types/Attributes | 4 | 0 | 4 |
| Temporal | ~8 | 0 | ~8 |
| Snapshots | 7 + engine | 0 | 7 + engine |
| @context hosting | 4 | 4 + persistence | 0 |
| **Total** | **~47** | **20 + sub engine + distops retrieve + ctx persistence** | **~27** |

### Rough Estimates to Full NGSI-LD v1.9.1 Coverage

| Phase | Scope | Estimate |
|-------|-------|----------|
| Entity CRUD (remaining 3 endpoints) | PUT entity + attr ops | ~1.5 weeks |
| Batch operations | 6 batch endpoints | ~1.5 weeks |
| Subscriptions + notifications | CRUD + notification engine + geo-fencing | ~3-4 weeks |
| Registrations + distributed ops | CRUD + forwarding engine | ~3-4 weeks |
| Type/attribute discovery | 4 endpoints | ~1 week |
| Temporal | Full temporal API | ~4-6 weeks |
| Snapshots | CRUD + async query engine + notifications | ~2-3 weeks |
| Missing query features | orderBy, join, entityMap, etc. | ~2-3 weeks |
| **Total** | | **~5-6 months** |

Note: Estimates assume a single developer, include functional tests for each
feature, and exclude ETSI conformance test suite alignment. Coverage target
is **ETSI GS CIM 009 v1.9.1** (published 2025-07). The upcoming v1.10 of the
spec will introduce additional features not accounted for here.
