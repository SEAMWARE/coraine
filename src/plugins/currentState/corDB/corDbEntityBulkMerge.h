#ifndef CORDB_CORDBENTITYBULKMERGE_H_
#define CORDB_CORDBENTITYBULKMERGE_H_

//
// FILE            corDbEntityBulkMerge.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
#include "kjson/KjNode.h"                               // KjNode
#include "corNgsild/ldEntityMerge.h"                     // LdMergeReport

#include "db/Tenant.h"                                  // Tenant



// -----------------------------------------------------------------------------
//
// corDbEntityBulkRetrieve - Batch Merge Phase 1: clone current stored entities
// into the request arena. `targetsV` is a caller-allocated, zeroed array
// parallel to the fragments; same-id fragments share one clone.
//
extern int corDbEntityBulkRetrieve(Tenant* tenantP, KjNode* fragmentsArr,
                                   KjNode** targetsV);



// -----------------------------------------------------------------------------
//
// corDbEntityBulkChangesApply - Batch Merge Phase 2: apply each fragment's
// change report to its live stored entity.
//
extern int corDbEntityBulkChangesApply(Tenant* tenantP, KjNode* fragmentsArr,
                                       KjNode** mergedTargetsV, LdMergeReport* reportsV,
                                       int* resultsV);

#endif  // CORDB_CORDBENTITYBULKMERGE_H_