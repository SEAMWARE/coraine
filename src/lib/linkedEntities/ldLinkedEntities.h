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
#include <stdbool.h>                                  // bool
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



// -----------------------------------------------------------------------------
//
// ldLinkedEntitiesExpandArrayFlat - flat-expand every primary in arrayP
//
// In-place: appends fetched targets to the same KjArray. The visited-
// set spans all primaries + all newly-fetched targets so a target
// shared by two primaries lands in the array exactly once (per spec
// § 4.5.23.3 — both linking and linked are joined into one array).
//
extern void ldLinkedEntitiesExpandArrayFlat(KjNode* arrayP, int joinLevel, Tenant* tenantP);



// -----------------------------------------------------------------------------
//
// ldLinkedEntitiesExpandArrayInline - inline-expand every primary in arrayP
//
// Each primary keeps its own visited-set so a target shared by two
// primaries is correctly inlined under both. The array length stays
// equal to the result count.
//
extern void ldLinkedEntitiesExpandArrayInline(KjNode* arrayP, int joinLevel, Tenant* tenantP);



// -----------------------------------------------------------------------------
//
// linkedFetchOne - single-entity lookup (local DB → reg-cache → dist-op GET)
//
// objectTypeV is a NULL-terminated array of the target's entity-type
// IRI(s) (a Relationship's objectType, already @vocab-expanded), or NULL
// when the type is unknown. When non-NULL it scopes the reg-cache match
// to those types so only the registrations that can serve the type are
// forwarded to.
//
// typedRemoteOnly is the § 7.7.1 linked-retrieval fan-out gate: when true,
// a target that is not stored locally is only fetched remotely if its type
// is known (objectTypeV != NULL) — without a type the broker leaves it as a
// bare object URI rather than spraying a GET at every registration. Pass
// false to keep the legacy fetch-by-id-regardless behaviour (q-filter
// evaluation, notification linked-entity inclusion).
//
// Returns 0 on success and *entityPP set to a storage-format tree
// allocated in the request arena. Non-zero on miss.
//
extern int linkedFetchOne(const char* entityId, char** objectTypeV, bool typedRemoteOnly, KjNode** entityPP, Tenant* tenantP);



// -----------------------------------------------------------------------------
//
// ldLinkedEntitiesNotifApiArray - flat / inline expand on an API-format array
//
// The notification path runs ldEntityToApi on each matched entity before
// adding it to the data[] array (datasetId wrappers unwrapped, "value"
// renamed back to "object" for Relationships, etc.). The above walkers
// expect storage shape; this entry-point operates on the post-API tree
// that lives in the notification body.
//
// flat   → fetched targets are appended to the array, each ldEntityToApi-
//          converted to match the existing entries.
// inline → each Relationship instance gets an "entity" sub-attribute
//          carrying the target tree (also API-converted).
//
extern void ldLinkedEntitiesNotifApiArray(KjNode* arrayP, const char* mode, int joinLevel, bool sysAttrs, Tenant* tenantP);

#endif  // LE_LDLINKEDENTITIES_H_
