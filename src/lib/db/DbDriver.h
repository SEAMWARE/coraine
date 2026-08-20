#ifndef DB_DBDRIVER_H_
#define DB_DBDRIVER_H_

//
// FILE            DbDriver.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//

#include <stdint.h>                                       // uint64_t
#include <stdbool.h>                                      // bool

#include "kalloc/KAlloc.h"                                // KAlloc
#include "kargs/KArg.h"                                   // KArg
#include "kjson/KjNode.h"                                 // KjNode

#include "corNgsild/ldEntityMerge.h"                       // LdMergeReport
#include "corNgsild/LdSubCache.h"                          // LdGeoMatchFunc
#include "corNgsild/LdRegCache.h"                          // LdCsrGeoMatchFunc

#include "db/DbQueryFilter.h"                             // DbQueryFilter
#include "db/Tenant.h"                                    // Tenant
#include "ha/HaEvent.h"                                   // HaApplyFunc



// -----------------------------------------------------------------------------
//
// Error codes
//
#define DB_OK                  0
#define DB_ERR                -1
#define DB_ALREADY_EXISTS     -2
#define DB_NOT_FOUND          -3
#define DB_INVALID_GEOMETRY   -4   // geometry is well-formed JSON but the storage
                                   // layer rejects it (e.g. mongo 2dsphere/S2
                                   // rejects polygons with self-intersection or
                                   // degenerate edges that GeoJSON itself allows)
#define DB_BAD_INPUT          -5   // user-supplied data was structurally accepted
                                   // by the API layer but rejected by the storage
                                   // layer's own validator (e.g. mongo's PCRE
                                   // regex rejects patterns that POSIX regcomp
                                   // accepted). Caller should respond 400, not 500.
#define DB_GEO_TYPE_CONFLICT  -6   // an Attribute name is already established in
                                   // this tenant as a GeoProperty and the write
                                   // uses it as something else, or the reverse.
                                   // A mongoc-only limit: its 2dsphere index is
                                   // per attribute PATH and collection-wide, so
                                   // one name cannot hold both kinds. Caller
                                   // should respond 409 Conflict. (ramdb has no
                                   // such index and accepts the mix.)



// -----------------------------------------------------------------------------
//
// Function pointer types
//
typedef int  (*DbInitFunc)(void);
typedef void (*DbCloseFunc)(void);

//
// JSON-LD context persistence (NGSI-LD § 5.13 Context Hosting).
//
// Context rows are stored in a fixed, reserved database (e.g. "coraine")
// independent of any tenant. Hosted, Cached, and ImplicitlyCreated contexts
// are persisted. ImplicitlyCreated covers contexts the broker mints on
// behalf of a request — most notably the one synthesised for a Subscription
// whose @context is an inline array or object (§ 5.5.5 / § 5.8.1.4): the
// resulting URL is stored as the subscription's `jsonldContext` and has to
// resolve again after a restart.
//
#define DB_CONTEXT_KIND_CACHED   1
#define DB_CONTEXT_KIND_HOSTED   2
#define DB_CONTEXT_KIND_IMPLICIT 3

typedef struct DbContextRow
{
  char*  id;     // canonical identifier (URL for Cached, urn:ngsi-ld:Context: for Hosted)
  char*  url;    // download URL (NULL for Hosted)
  int    kind;   // DB_CONTEXT_KIND_*
  char*  body;   // raw JSON body (the @context document)
} DbContextRow;

typedef int (*DbContextSaveFunc)(const char* id, const char* url, int kind, const char* body);
typedef int (*DbContextDeleteFunc)(const char* id);
typedef int (*DbContextListFunc)(KAlloc* allocP, DbContextRow** rowsPP, int* countP);
typedef int (*DbContextGetFunc)(const char* id, KAlloc* allocP, DbContextRow* rowOut);

typedef int  (*DbEntityCreateFunc)(Tenant* tenantP, const char* entityId, KjNode* entityP);

//
// DbEntityBulkCreateFunc - batch insert of N entities in a single DB
// round-trip where the driver supports it (mongoc: insert_many).
//
// entitiesArr is a KjArray of DB-format entities (each already an object
// with _id / type / attrs wrappers — ldApiEntityToDbModel already ran).
//
// resultsV is a caller-allocated int[N], populated per-entity with one
// of: DB_OK on success, DB_ALREADY_EXISTS on duplicate id, DB_ERR on
// anything else. N must equal the number of entities in entitiesArr.
//
// Return value is DB_OK if at least one entity succeeded, otherwise
// DB_ERR. The per-entity outcome is carried in resultsV; callers map
// that to the BatchOperationResult body.
//
typedef int  (*DbEntityBulkCreateFunc)(Tenant* tenantP, KjNode* entitiesArr, int* resultsV);

//
// DbEntityBulkUpdateFunc - batch replace of N already-merged entities in a
// single DB round-trip where the driver supports it (mongoc: bulk_write
// with replace_one operations).
//
// entitiesArr is a KjArray of DB-format entities — each one the final
// merged state as computed by the service routine (post multi-instance
// merge). The caller has already run ldApiEntityToDbModel and each
// entity carries its id (or _id).
//
// resultsV is a caller-allocated int[N] populated per-entity with one
// of: DB_OK on success, DB_NOT_FOUND if the id doesn't exist (batch
// update requires pre-existence), DB_ERR on anything else.
//
typedef int  (*DbEntityBulkUpdateFunc)(Tenant* tenantP, KjNode* entitiesArr, int* resultsV);

//
// Batch Merge (§ 5.6.10) is done in two driver calls bracketing the merge,
// which the broker performs itself (ldEntityMerge) — so the merge engine lives
// in one place and a new DB plugin only implements these two storage primitives:
//
// DbEntityBulkRetrieveFunc - fetch the current DB-form trees for a batch of
// fragments in one round-trip where the driver supports it (mongoc: one $in).
// targetsV is a caller-allocated, zeroed KjNode*[N] parallel to fragmentsArr;
// each slot receives the request-arena tree of the current entity, or stays
// NULL when the id does not exist. Fragments that share an id share ONE target
// tree so the broker's sequential merges accumulate (array-order semantics).
//
typedef int  (*DbEntityBulkRetrieveFunc)(Tenant* tenantP, KjNode* fragmentsArr,
                                         KjNode** targetsV);

//
// DbEntityBulkChangesApplyFunc - persist a batch of already-merged entities.
// The broker has run ldEntityMerge over each targetsV tree, producing
// reportsV[i] (the per-fragment change record, also used for subscription
// notification matching). This stages one surgical update per changed entity
// (mongoc: a single bulk_write) and executes it.
//
// resultsV is a caller-allocated int[N]: DB_OK for slots the broker merged
// (others are skipped here); staged slots may be demoted to DB_ERR on failure.
// mergedTargetsV are the post-merge trees (= targetsV from the retrieve).
//
typedef int  (*DbEntityBulkChangesApplyFunc)(Tenant* tenantP, KjNode* fragmentsArr,
                                             KjNode** mergedTargetsV,
                                             LdMergeReport* reportsV, int* resultsV);

typedef int  (*DbEntityRetrieveFunc)(Tenant* tenantP, const char* entityId, KjNode** entityPP);
typedef int  (*DbEntityQueryFunc)(Tenant* tenantP, DbQueryFilter* filterP, KjNode** arrayPP);
typedef int  (*DbEntityDeleteFunc)(Tenant* tenantP, const char* entityId);

//
// DbEntityBulkDeleteFunc - batch delete (§ 5.6.11) of N entity ids.
//
// idV is a caller-provided char*[N]; resultsV[N] is populated per id
// with DB_OK / DB_NOT_FOUND / DB_ERR. snapshotsV[N] is populated on
// DB_OK with the pre-delete entity (request-arena lifetime via the
// broker's active allocator) so the service can fire delete
// notifications without an extra retrieve.
//
// mongoc impl does one $in fetch + one bulk_write of delete_one ops
// (2 round-trips total). corDB just loops.
//
typedef int  (*DbEntityBulkDeleteFunc)(Tenant* tenantP, const char** idV, int N,
                                       int* resultsV, KjNode** snapshotsV);
//
// DbEntityChangesApplyFunc - persist a merged single entity (Merge Entity §
// 10.2.9 / Partial Attribute Update § 10.2.5). The broker has already merged
// `mergedEntity` in memory (ldEntityMerge / ldEntityFragmentApply, against the
// tree from db.entityRetrieve) and produced `reportP` describing which
// top-level attributes were created/modified/deleted. The driver translates the
// report into a surgical write (mongoc: $set/$unset) — no merge logic here.
//
typedef int  (*DbEntityChangesApplyFunc)(Tenant* tenantP, const char* entityId,
                                         KjNode* mergedEntity, LdMergeReport* reportP);
typedef int  (*DbEntityReplaceFunc)(Tenant* tenantP, const char* entityId,
                                    KjNode* newEntityP, KjNode** oldEntityPP);
//
// DbEntityAttrsSetFunc - generic "set attrs/dsKeys on an entity".
//
// For each top-level attribute in fragmentP, and for each dsKey-keyed
// instance within, set it on the target entity:
//   - if target has a matching (attrName, dsKey) → replace the instance
//     preserving createdAt;
//   - otherwise → append the instance.
// In both cases bump the entity's and touched attr's modifiedAt to ts.
//
// Handles "type" (union into target's type array) and "scope" (replaced
// when overwriteScope is true, union-appended otherwise) at top level.
//
// Fragment is in storage (DB-model) format — the caller has already run
// ldApiEntityToDbModel on it. reportP is filled for subscription
// matching; pass NULL to skip reporting.
//
// Returns DB_OK / DB_NOT_FOUND / DB_ERR.
//
typedef int  (*DbEntityAttrsSetFunc)(Tenant* tenantP, const char* entityId,
                                     KjNode* fragmentP, bool overwriteScope,
                                     uint64_t ts, LdMergeReport* reportP);

//
// DbTypeListFunc - aggregate distinct entity types and their attribute sets.
//
// Returns a KjArray of objects, each:
//   { "typeIri":    "<IRI>",
//     "attrs":      [ "<attrIri>", ... ],
//     "attrTypes":  { "<attrIri>": [ "Property", "Relationship", ... ] },
//     "entityCount": <int> }
//
// attrTypes and entityCount are only populated when details is true —
// the simple list case short-circuits the heavier per-type scan.
//
// Nodes are allocated via corRest.kjsonP.
//
typedef int  (*DbTypeListFunc)(Tenant* tenantP, bool details, KjNode** arrayPP);

//
// DbAttrListFunc - aggregate distinct attribute names and the entity
// types they appear on.
//
// Returns a KjArray of objects, each:
//   { "attrIri":       "<IRI>",
//     "typeNames":     [ "<typeIri>", ... ],
//     "attrTypes":     [ "Property", "Relationship", ... ],
//     "attrCount":     <int> }
//
typedef int  (*DbAttrListFunc)(Tenant* tenantP, bool details, KjNode** arrayPP);
typedef int  (*DbSubscriptionCreateFunc)(Tenant* tenantP, const char* subId, KjNode* subP);
typedef int  (*DbSubscriptionRetrieveFunc)(Tenant* tenantP, const char* subId, KjNode** subPP);
typedef int  (*DbSubscriptionQueryFunc)(Tenant* tenantP, int limit, int offset, KjNode** arrayPP);
typedef int  (*DbSubscriptionUpdateFunc)(Tenant* tenantP, const char* subId, KjNode* fragmentP);
typedef int  (*DbSubscriptionReplaceFunc)(Tenant* tenantP, const char* subId, KjNode* subP);
typedef int  (*DbSubscriptionDeleteFunc)(Tenant* tenantP, const char* subId);
typedef KjNode* (*DbSubscriptionListFunc)(Tenant* tenantP);

// Subscription stats flush — HA-safe: deltas are added via $inc (never
// read-modify-write), "last*" timestamps are set unconditionally.
//   deltaSent + deltaFailed: 0 is valid (will still update last* if set).
//   lastNotification/Success/Failure: 0 means "don't update this field".
typedef int  (*DbSubscriptionStatsFlushFunc)(Tenant*      tenantP,
                                             const char*  subId,
                                             int          deltaSent,
                                             int          deltaFailed,
                                             uint64_t     lastNotification,
                                             uint64_t     lastSuccess,
                                             uint64_t     lastFailure);

typedef int  (*DbRegistrationCreateFunc)(Tenant* tenantP, const char* regId, KjNode* regP);
typedef int  (*DbRegistrationRetrieveFunc)(Tenant* tenantP, const char* regId, KjNode** regPP);
typedef int  (*DbRegistrationQueryFunc)(Tenant* tenantP, int limit, int offset, KjNode** arrayPP);
typedef int  (*DbRegistrationUpdateFunc)(Tenant* tenantP, const char* regId, KjNode* fragmentP);
typedef int  (*DbRegistrationDeleteFunc)(Tenant* tenantP, const char* regId);
typedef KjNode* (*DbRegistrationListFunc)(Tenant* tenantP);

//
// Snapshot persistence (§ 5.16). The snapshot's *metadata* (tree:
// id, snapshotQueries, snapshotStatus, timestamps, priority,
// snapshotQueriesDetails, plus a hidden "_snapSeq" used to rebuild
// the per-snapshot tenant DB name on boot) lives in the originating
// tenant's "snapshots" collection. Entity bodies live in the
// per-snapshot tenant DB (see snapshotTenant.{c,h}).
//
typedef int  (*DbSnapshotCreateFunc)(Tenant* tenantP, const char* snapId, KjNode* snapP);
typedef int  (*DbSnapshotQueryFunc)(Tenant* tenantP, KjNode** arrayPP);
typedef int  (*DbSnapshotUpdateFunc)(Tenant* tenantP, const char* snapId, KjNode* fragmentP);
typedef int  (*DbSnapshotDeleteFunc)(Tenant* tenantP, const char* snapId);

//
// DbTenantDropFunc - drop the entire per-tenant DB. Used by snapshot
// cleanup (delete/purge) to reclaim the snap-tenant's storage. NULL
// allowed for plugins that don't persist (ramdb): caller treats a
// NULL function pointer as a no-op.
//
typedef int  (*DbTenantDropFunc)(Tenant* tenantP);

typedef int  (*DbTenantSetupFunc)(Tenant* tenantP);
typedef void (*DbVersionInfoFunc)(KAlloc* allocP, KjNode* root);

//
// DbHaWatchStartFunc - start watching the store for what OTHER broker instances
// write, turning each change into an HaEvent + haEventApply() (see src/lib/ha).
//
// The watch is the store's own mechanism, which is why it lives in the driver
// and not in the broker: mongo has change streams, another store would have
// something else, and a store that is not shared with the other instances (a
// process-local one) has nothing to report at all. NULL means exactly that, and
// '--ha mongo' then refuses to start.
//
// Called once, after init(), only when --ha names this channel. Returns once the
// watch is running - it does its work in a thread of its own.
//
typedef int  (*DbHaWatchStartFunc)(HaApplyFunc applyF);



// -----------------------------------------------------------------------------
//
// DbDriver -
//
typedef struct DbDriver
{
  const char*             alias;           // short plugin name, e.g. "mongoc" (for usage text)
  const char*             version;         // plugin version string
  KArg*                   args;            // plugin-contributed CLI args (NULL if none)
  DbInitFunc              init;
  DbCloseFunc             close;
  DbEntityCreateFunc      entityCreate;
  DbEntityBulkCreateFunc  entityBulkCreate;
  DbEntityBulkUpdateFunc  entityBulkUpdate;
  DbEntityBulkRetrieveFunc     entityBulkRetrieve;
  DbEntityBulkChangesApplyFunc entityBulkChangesApply;
  DbEntityBulkDeleteFunc  entityBulkDelete;
  DbEntityRetrieveFunc    entityRetrieve;
  DbEntityQueryFunc       entityQuery;
  DbEntityDeleteFunc      entityDelete;
  DbEntityChangesApplyFunc entityChangesApply;
  DbEntityReplaceFunc     entityReplace;
  DbEntityAttrsSetFunc    entityAttrsSet;
  DbTypeListFunc          typeList;        // Discovery § 5.7.5 / § 5.7.6 / § 5.7.7
  DbAttrListFunc          attrList;        // Discovery § 5.7.8 / § 5.7.9 / § 5.7.10
  DbSubscriptionCreateFunc   subscriptionCreate;
  DbSubscriptionRetrieveFunc subscriptionRetrieve;
  DbSubscriptionQueryFunc    subscriptionQuery;
  DbSubscriptionUpdateFunc   subscriptionUpdate;   // partial field-patch (subordinates, status)
  DbSubscriptionReplaceFunc  subscriptionReplace;  // full-document store (user PATCH; broker owns the merge)
  DbSubscriptionDeleteFunc   subscriptionDelete;
  DbSubscriptionListFunc     subscriptionList;
  DbSubscriptionStatsFlushFunc subscriptionStatsFlush; // NULL-allowed (e.g. ramdb)
  DbRegistrationCreateFunc   registrationCreate;
  DbRegistrationRetrieveFunc registrationRetrieve;
  DbRegistrationQueryFunc    registrationQuery;
  DbRegistrationUpdateFunc   registrationUpdate;
  DbRegistrationDeleteFunc   registrationDelete;
  DbRegistrationListFunc     registrationList;
  DbSnapshotCreateFunc       snapshotCreate;       // NULL-allowed (ramdb)
  DbSnapshotQueryFunc        snapshotQuery;        // NULL-allowed
  DbSnapshotUpdateFunc       snapshotUpdate;       // NULL-allowed
  DbSnapshotDeleteFunc       snapshotDelete;       // NULL-allowed
  DbTenantDropFunc           tenantDrop;           // NULL-allowed
  DbTenantSetupFunc       tenantSetup;
  DbVersionInfoFunc       versionInfo;
  DbHaWatchStartFunc      haWatchStart;    // NULL-allowed (e.g. ramdb) — see the typedef
  LdGeoMatchFunc       geoMatchFunc;    // geo match callback for subscription notifications
  LdCsrGeoMatchFunc    csrGeoMatchFunc; // geoQ ↔ CSR geo-coverage match (§ 5.10.2.4 / dispatch)

  // JSON-LD context persistence — optional; NULL means no persistence
  // (e.g. ramdb). The reserved DB name ("coraine") is used internally.
  DbContextSaveFunc       contextSave;
  DbContextDeleteFunc     contextDelete;
  DbContextListFunc       contextList;
  DbContextGetFunc        contextGet;
} DbDriver;



// -----------------------------------------------------------------------------
//
// DbRegisterFunc -
//
typedef void (*DbRegisterFunc)(DbDriver* driverP);



// -----------------------------------------------------------------------------
//
// dbDriver - global driver instance
//
extern DbDriver db;

#endif  // DB_DBDRIVER_H_
