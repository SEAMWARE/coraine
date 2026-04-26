#ifndef LE_LDLINKEDENTITIES_H_
#define LE_LDLINKEDENTITIES_H_

//
// FILE            ldLinkedEntities.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//
// NGSI-LD § 4.5.23 — linked-entity retrieval. Walk the primary
// entity's Relationships, fetch target entities, recurse up to
// `joinLevel`, dedupe via a visited-set so cycles terminate.
//
// Output formats per § 4.5.23.2 / .3:
//   * "flat"   → array: primary first, then every retrieved target.
//   * "inline" → primary only; each Relationship gets a sub-attribute
//                "entity" carrying the target's tree. (Slice C; not
//                yet implemented.)
//
#include "kjson/KjNode.h"                            // KjNode

#include "db/Tenant.h"                               // Tenant



// -----------------------------------------------------------------------------
//
// ldLinkedEntitiesFlat - § 4.5.23.3 flat representation
//
// primaryP   : the primary entity tree, in storage format (post-DB
//              fetch, pre-ldEntityToApi). Becomes the first element
//              of the returned array.
// joinLevel  : 1 = follow primary's Relationships only; N+1 = also
//              follow the targets' Relationships, etc.
// tenantP    : tenant for db.entityRetrieve calls.
//
// Returns a KjArray allocated in swRest.kjsonP. Entities the broker
// does not hold locally are skipped (per § 4.5.23 "limited to avoid
// cascades"). distOps integration is a follow-up slice.
//
extern KjNode* ldLinkedEntitiesFlat(KjNode* primaryP, int joinLevel, Tenant* tenantP);



// -----------------------------------------------------------------------------
//
// ldLinkedEntitiesInline - § 4.5.23.2 inline representation
//
// Attaches each fetched target as an "entity" sub-attribute on the
// originating Relationship instance, recursing up to joinLevel. Returns
// the primary (mutated in place); the response stays a single object,
// with linked targets nested. Visited-set + missing-target rules match
// the flat variant.
//
// To keep ldEntityToApi (run by renderHook on the primary) idempotent,
// the walker pre-converts every inlined target to API format itself —
// the primary is left in storage so the renderHook converts it once.
//
extern KjNode* ldLinkedEntitiesInline(KjNode* primaryP, int joinLevel, Tenant* tenantP);

#endif  // LE_LDLINKEDENTITIES_H_
