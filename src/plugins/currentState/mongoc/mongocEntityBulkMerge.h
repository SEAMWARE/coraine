#ifndef MONGOC_MONGOCENTITYBULKMERGE_H_
#define MONGOC_MONGOCENTITYBULKMERGE_H_

//
// FILE            mongocEntityBulkMerge.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//
#include "kjson/KjNode.h"                             // KjNode
#include "swNgsild/ldEntityMerge.h"                   // LdMergeReport

#include "db/Tenant.h"                                 // Tenant



// -----------------------------------------------------------------------------
//
// mongocEntityBulkRetrieve - Batch Merge Phase 1: one $in fetch of all current
// docs into the request arena. `targetsV` is a caller-allocated, zeroed array
// parallel to the fragments; same-id fragments share one target tree.
//
extern int mongocEntityBulkRetrieve(Tenant* tenantP, KjNode* fragmentsArr,
                                    KjNode** targetsV);



// -----------------------------------------------------------------------------
//
// mongocEntityBulkChangesApply - Batch Merge Phase 2/3: stage one surgical
// update per already-merged target (from its report) into a single bulk
// operation and execute it. resultsV[i] is DB_OK for slots the broker merged;
// staged slots are demoted to DB_ERR on bulk-execute failure.
//
extern int mongocEntityBulkChangesApply(Tenant* tenantP, KjNode* fragmentsArr,
                                        KjNode** mergedTargetsV, LdMergeReport* reportsV,
                                        int* resultsV);

#endif  // MONGOC_MONGOCENTITYBULKMERGE_H_
