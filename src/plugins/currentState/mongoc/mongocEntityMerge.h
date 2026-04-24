#ifndef MONGOC_MONGOCENTITYMERGE_H_
#define MONGOC_MONGOCENTITYMERGE_H_

//
// FILE            mongocEntityMerge.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//
#include <stdint.h>                                   // uint64_t

#include <stdbool.h>                                  // bool

#include <mongoc/mongoc.h>                            // mongoc_collection_t

#include "kjson/KjNode.h"                             // KjNode
#include "swNgsild/ldEntityMerge.h"                   // LdMergeReport

#include "db/Tenant.h"                                 // Tenant



// -----------------------------------------------------------------------------
//
// mongocEntityMergeBuildUpdate - merge fragmentDb into an already-fetched
// target tree and build a $set/$unset bson body (without executing it).
//
// Used by the batch-merge path, which wants to stage many updates into a
// single bulk operation — it has already fetched N current docs via one
// $in query, so it doesn't want this helper to fetch again.
//
// On DB_OK, *updateDocOut is initialised with the update body.
// *noChangesOut is set true when ldEntityMerge succeeded but produced no
// writes (caller should skip the bulk op for this entity). Caller owns
// bson_destroy(updateDocOut).
//
extern int mongocEntityMergeBuildUpdate(KjNode*              target,
                                        KjNode*              fragmentDb,
                                        uint64_t             ts,
                                        LdMergeReport*       reportP,
                                        bson_t*              updateDocOut,
                                        bool*                noChangesOut);



// -----------------------------------------------------------------------------
//
// mongocEntityMergeOne - apply Merge Entity on an already-opened collection.
//
// Reusable core for both the single-entity PATCH and the future Batch Merge
// (POST /entityOperations/merge). The caller owns the collection handle.
//
// Current implementation: fetch → ldEntityMerge → replace_one. This will be
// upgraded to walk the fragment and emit $set / $unset at specific dotted
// paths so the wire write is per-changed-attribute rather than the whole
// document. Until then, a PATCH that touches one attribute on a large entity
// still transfers the full entity.
//
// targetPP (optional, may be NULL) is populated on DB_OK with the
// post-merge target tree (request-arena lifetime via swRest.kalloc) —
// used by Batch Merge to skip the extra retrieve for notifications.
//
extern int mongocEntityMergeOne(mongoc_collection_t* collP,
                                const char*          entityId,
                                KjNode*              fragmentDb,
                                uint64_t             ts,
                                LdMergeReport*       reportP,
                                KjNode**             targetPP);



// -----------------------------------------------------------------------------
//
// mongocEntityMerge - DB driver entry point (single entity)
//
extern int mongocEntityMerge(Tenant*        tenantP,
                             const char*    entityId,
                             KjNode*        fragmentDb,
                             uint64_t       ts,
                             LdMergeReport* reportP);

#endif  // MONGOC_MONGOCENTITYMERGE_H_
