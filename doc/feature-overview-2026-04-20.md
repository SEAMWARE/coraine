# coraine — as of April 20, 2026

Lightweight NGSI-LD Context Broker in C. Plugin architecture for DB
(corRamDB in-memory, mongoc/MongoDB) and API extensions.
Spec target: **ETSI GS CIM 009 v1.9.1**.

---

## Entity CRUD

| Operation | Endpoint | Status |
|-----------|----------|--------|
| Create Entity | POST /entities | Done — local |
| Retrieve Entity | GET /entities/{id} | Done — local + full distops (all 4 modes) |
| Query Entities | GET /entities | Done — local + full distops (no-split + split, all 4 modes, EntityMap pagination) |
| Delete Entity | DELETE /entities/{id} | Done — local |
| Merge Entity | PATCH /entities/{id} | Done — local |
| Replace Entity | PUT /entities/{id} | Done — local |

- All 9 attribute types (incl. TemporalProperty)
- Sub-attributes at arbitrary depth
- Multi-entity-type (type as array)
- Multi-attribute (datasetId)
- Scope (entity scope property, scopeQ filtering)
- Three input/output formats: normalized, concise, simplified

## Query Features

| Feature | Status |
|---------|--------|
| q filter (full query language) | Done |
| scopeQ filter | Done |
| Geo-query (georel, geometry, coordinates, geoproperty) | Done |
| pick / omit (attribute projection) | Done |
| datasetId filter | Done |
| lang (LanguageProperty reduction) | Done |
| Pagination (limit, offset, count, Link headers) | Done |
| Output: normalized, concise, simplified (keyValues) | Done |
| GeoJSON (application/geo+json) | Done |
| sysAttrs (createdAt / modifiedAt) | Done |
| expandValues, scope | Done |
| orderBy (multi-key, asc/desc, missing-sorts-last) | Done |
| collation (BCP47 locale-aware string sort) | Done |
| join, entityMap | Not yet |

## Multi-Tenancy

Full support via `NGSILD-Tenant` header. Per-tenant DB isolation,
per-tenant subscription/registration caches.

## Subscriptions

Full CRUD + real-time notification engine. Matching on entity selector
(id, idPattern, type), q-filter, geoQ (GEOS), scopeQ. Throttling,
expiration, format (normalized/concise/simplified), counters, mongoc
persistence. datasetId instance filtering in notifications. Periodic
notifications (timeInterval) via separate pernot cache + background
loop thread. 80+ functional tests.

## Context Source Registrations + Distributed Operations

| Feature | Status |
|---------|--------|
| CSR CRUD (POST/GET/PATCH/DELETE) | Done |
| Creation-time conflict checks (§ 5.9.2) | Done |
| Forwarding plugin architecture (HTTP default, extensible) | Done |
| Exclusive mode — single authoritative upstream | Done |
| Redirect mode — strip local, multiple sources, merge per § 4.5.5.3 | Done |
| Inclusive mode — multi-source merge per § 4.5.5.3 | Done |
| Auxiliary mode — fill gaps only, never overrides | Done |
| Registration-constrained forwarding (pick= from reg attrs) | Done |
| `?type=` disambiguation on forward | Done |
| Via header + tenant-scoped loop detection | Done |
| `?local=true` to bypass forwarding | Done |
| Per-RegistrationInfo dispatch (respects info[] coverage areas) | Done |
| EntityMap creation (`entityMap=true` → frozen ID list) | Done |
| EntityMap CRUD (GET/DELETE /entityMaps/{id}) | Done |
| Discovery filter (§ 5.10.2) | 501 (deferred) |

## JSON-LD Context Handling

Core context v1.9 built in.
User @context via Link header or inline payload.
Context hosting (§ 5.13): all four CRUD endpoints, three context kinds,
mongoc persistence, concurrent-download dedup.

## Admin API

Health, version, log control, tenant listing, plugin listing.

---

## Performance

Measured 2026-04-15 on 20-core laptop. wrk -t4 -c50 -d5s, median of 3
runs. 5-attr Vehicle entity (~600B). GET returns 20 entities per page.

### Requests/second (higher is better)

| Scenario | Coraine ramdb | Coraine mongoc |
|----------|-------------:|--------------:|
| CREATE | 73k | 35k |
| PATCH | 289k | 30k |
| GET (ent/s) | 7.5M | 745k |
| DELETE | 355k | 16k |
| NOTIFY (rps) | 4.6k | 5.1k |

**Key takeaways:**
- **In-memory (ramdb)**: the fast path — CREATE/GET/DELETE all run several
  times the mongoc figures
- **MongoDB**: PATCH/DELETE are the weak spots (no batch pipeline yet)
- **GET throughput**: 7.5M entities/s on ramdb
- **NOTIFY**: under investigation

### Projected: binary forwarding protocol for distributed ops

The forwarding plugin architecture is designed for a future binary
transport (`corBin://`) between trusted broker instances. HTTP forwarding
pays per hop: JSON render + HTTP framing + JSON parse + @context
expand/compact — twice (request + response). A binary protocol
eliminates all four layers and transmits the in-memory entity tree
directly.

Estimated improvement for the **forwarding path**: **3–5x** compared to
HTTP-based forwarding. End-to-end
improvement for a distributed retrieveEntity depends on the ratio of
forwarding cost to local DB + merge cost, but for multi-hop or
multi-source queries the forwarding overhead dominates — making the
binary path a significant multiplier.

---

## Not Yet Implemented

- Attribute-level CRUD (POST/PATCH/DELETE/PUT individual attrs)
- Batch operations (entityOperations/*)
- Type/attribute discovery (GET /types, /attributes)
- Temporal API (TRoE)
- Snapshots (§ 5.16)
- MQTT notification transport
- join / joinLevel (linked entity graph traversal)
- DistOps for write operations

### Estimated path to full NGSI-LD v1.9.1 conformance

| Remaining area | Estimate |
|----------------|----------|
| Attribute-level CRUD (4 endpoints) | 1 week |
| Batch operations (6 endpoints) | 1.5 weeks |
| MQTT notifications | 1 week |
| DistOps for write operations | 1–2 weeks |
| Type/attribute discovery (4 endpoints) | 1 week |
| Temporal API | 4–6 weeks |
| Snapshots (§ 5.16) | 2–3 weeks |
| join / joinLevel | 1 week |
| ETSI conformance test alignment | 2–3 weeks |
| **Total** | **~3.5 months** |

Single-developer estimate including functional tests per feature.
Temporal API and ETSI conformance alignment are the largest blocks.
