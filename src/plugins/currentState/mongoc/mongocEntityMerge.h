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

#include <mongoc/mongoc.h>                            // bson_t

#include "kjson/KjNode.h"                             // KjNode
#include "swNgsild/ldEntityMerge.h"                   // LdMergeReport

#include "db/Tenant.h"                                 // Tenant



// -----------------------------------------------------------------------------
//
// mongocBuildSurgicalUpdate - translate a merge report into a $set/$unset body.
//
// `mergedEntity` is the already-merged in-memory tree (the broker ran the merge
// engine); `reportP` lists which top-level attributes changed. Appends into the
// caller-initialised `updateDocOut`. *noChangesOut is set true when there is
// nothing to write. Shared by the single-entity and batch persist paths.
//
extern void mongocBuildSurgicalUpdate(KjNode*        mergedEntity,
                                      LdMergeReport* reportP,
                                      bson_t*        updateDocOut,
                                      bool*          noChangesOut);



// -----------------------------------------------------------------------------
//
// mongocEntityChangesApply - persist a merged single entity (DB driver entry).
//
// The broker has already merged `mergedEntity` and produced `reportP`; this
// builds the surgical update and runs one update_one.
//
extern int mongocEntityChangesApply(Tenant* tenantP, const char* entityId,
                                    KjNode* mergedEntity, LdMergeReport* reportP);

#endif  // MONGOC_MONGOCENTITYMERGE_H_
