# swBroker Implementation Status

Version: post-0.2.0
Date: 2026-04-10

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

**Status: Complete**

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
| 201 Created + Location header | Done |
| 409 AlreadyExists | Done |
| Error handling (missing id, missing type, bad payload, bad Content-Type) | Done |
| Subscriptions / notifications on create | Not done |
| Distributed operations (forwarding to registrations) | Not done |

Functional tests: `create_entity*.test` (9 test files), `tenant.test`, `tenant_persistence.test`

### 2. GET /ngsi-ld/v1/entities — Query Entities

**Status: Mostly complete (local queries)**

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
| orderBy, orderFrom, orderGeometry, collation | Not done |
| join, joinLevel, containedBy | Not done |
| entityMap, entityMapLifetime, splitEntities | Not done |
| local | Not done |
| csf | Not done |
| Distributed operations | Not done |

Functional tests: `query_entities_*.test` (20 test files), `tenant_geo.test`, `geojson_response.test`, `url_param_expand_values.test`

### 3. GET /ngsi-ld/v1/entities/{entityId} — Retrieve Entity

**Status: Basic implementation**

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
| type (entity type selection for disambiguation) | Not done |
| join, joinLevel, containedBy | Not done |
| entityMap, entityMapLifetime | Not done |
| Distributed operations | Not done |

Functional tests: `retrieve_entity_*.test` (5 test files), `geojson_response.test`

### 4. Admin API (plugin)

**Status: Complete**

- GET /admin/health
- GET /admin/version
- GET/PUT/POST/PATCH/DELETE /admin/log (verbose, debug, info, traceLevels)
- GET /admin/tenants
- GET /admin/plugins

### 5. Other

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
| DELETE /entities/{entityId} | 5.5.5 | Low | 1 day |
| PUT /entities/{entityId} | 5.5.3 | Medium | 2 days |
| PATCH /entities/{entityId} | 5.5.6 | Medium | 2-3 days |
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

| Endpoint | Spec Section | Complexity | Estimate |
|----------|-------------|------------|----------|
| POST /subscriptions | 5.8.1 | High | 5-7 days |
| GET /subscriptions | 5.8.4 | Low | 1 day |
| GET /subscriptions/{id} | 5.8.3 | Low | 0.5 days |
| PATCH /subscriptions/{id} | 5.8.2 | Medium | 2-3 days |
| DELETE /subscriptions/{id} | 5.8.5 | Low | 0.5 days |
| Notification engine (HTTP/MQTT) | 5.10 | High | 5-7 days |
| Geo-fencing (in-memory matching) | 5.10 | High | 3-5 days |

### Registration / Distributed Operations

| Endpoint | Spec Section | Complexity | Estimate |
|----------|-------------|------------|----------|
| POST /csourceRegistrations | 5.9.1 | Medium | 2-3 days |
| GET /csourceRegistrations | 5.9.4 | Medium | 2 days |
| GET /csourceRegistrations/{id} | 5.9.3 | Low | 0.5 days |
| PATCH /csourceRegistrations/{id} | 5.9.2 | Medium | 2 days |
| DELETE /csourceRegistrations/{id} | 5.9.5 | Low | 0.5 days |
| Distributed operation forwarding | 5.11 | Very High | 10-15 days |

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

| Endpoint | Spec Section | Complexity | Estimate |
|----------|-------------|------------|----------|
| GET /jsonldContexts | 5.13 | Low | 1 day |
| POST /jsonldContexts | 5.13 | Low | 1 day |
| GET /jsonldContexts/{id} | 5.13 | Low | 0.5 days |
| DELETE /jsonldContexts/{id} | 5.13 | Low | 0.5 days |

---

## DB Driver Interface

Currently supports 3 operations:
- `entityCreate` — used by POST /entities
- `entityRetrieve` — used by GET /entities/{id}
- `entityQuery` — used by GET /entities

New endpoints will require extending the `DbDriver` interface with:
- `entityDelete`
- `entityReplace` (PUT)
- `entityMergePatch` (PATCH)
- `entityAppendAttrs` (POST attrs)
- `entityUpdateAttrs` (PATCH attrs)
- `attrUpdate` (PATCH attr)
- `attrDelete` (DELETE attr)
- `attrReplace` (PUT attr)
- Subscription CRUD
- Registration CRUD
- Temporal CRUD (if supported)

---

## Summary

| Category | Endpoints | Done | Remaining |
|----------|-----------|------|-----------|
| Entity CRUD | 8 | 3 | 5 |
| Batch Operations | 6 | 0 | 6 |
| Subscriptions | 5 + engine | 0 | 5 + engine |
| Registrations | 5 + distops | 0 | 5 + distops |
| Types/Attributes | 4 | 0 | 4 |
| Temporal | ~8 | 0 | ~8 |
| Snapshots | 7 + engine | 0 | 7 + engine |
| @context hosting | 4 | 0 | 4 |
| **Total** | **~47** | **3** | **~44** |

### Rough Estimates to Full NGSI-LD v1.9.1 Coverage

| Phase | Scope | Estimate |
|-------|-------|----------|
| Entity CRUD (remaining 5 endpoints) | DELETE, PUT, PATCH entity + attr ops | ~2 weeks |
| Batch operations | 6 batch endpoints | ~1.5 weeks |
| Subscriptions + notifications | CRUD + notification engine + geo-fencing | ~3-4 weeks |
| Registrations + distributed ops | CRUD + forwarding engine | ~3-4 weeks |
| Type/attribute discovery | 4 endpoints | ~1 week |
| Temporal | Full temporal API | ~4-6 weeks |
| Snapshots | CRUD + async query engine + notifications | ~2-3 weeks |
| Missing query features | orderBy, join, entityMap, etc. | ~2-3 weeks |
| @context hosting | 4 endpoints | ~0.5 weeks |
| **Total** | | **~5-6 months** |

Note: Estimates assume a single developer, include functional tests for each
feature, and exclude ETSI conformance test suite alignment. Coverage target
is **ETSI GS CIM 009 v1.9.1** (published 2025-07). The upcoming v1.10 of the
spec will introduce additional features not accounted for here.
