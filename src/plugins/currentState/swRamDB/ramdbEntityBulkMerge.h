#ifndef SWRAMDB_RAMDBENTITYBULKMERGE_H_
#define SWRAMDB_RAMDBENTITYBULKMERGE_H_

//
// FILE            ramdbEntityBulkMerge.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//
#include "kjson/KjNode.h"                               // KjNode
#include "swNgsild/ldEntityMerge.h"                     // LdMergeReport

#include "db/Tenant.h"                                  // Tenant



// -----------------------------------------------------------------------------
//
// ramdbEntityBulkRetrieve - Batch Merge Phase 1: clone current stored entities
// into the request arena. `targetsV` is a caller-allocated, zeroed array
// parallel to the fragments; same-id fragments share one clone.
//
extern int ramdbEntityBulkRetrieve(Tenant* tenantP, KjNode* fragmentsArr,
                                   KjNode** targetsV);



// -----------------------------------------------------------------------------
//
// ramdbEntityBulkChangesApply - Batch Merge Phase 2: apply each fragment's
// change report to its live stored entity.
//
extern int ramdbEntityBulkChangesApply(Tenant* tenantP, KjNode* fragmentsArr,
                                       KjNode** mergedTargetsV, LdMergeReport* reportsV,
                                       int* resultsV);

#endif  // SWRAMDB_RAMDBENTITYBULKMERGE_H_
