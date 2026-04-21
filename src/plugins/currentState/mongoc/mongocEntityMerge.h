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

#include <mongoc/mongoc.h>                            // mongoc_collection_t

#include "kjson/KjNode.h"                             // KjNode
#include "swNgsild/ldEntityMerge.h"                   // LdMergeReport

#include "db/Tenant.h"                                 // Tenant



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
