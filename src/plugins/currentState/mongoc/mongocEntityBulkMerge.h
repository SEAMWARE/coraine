#ifndef MONGOC_MONGOCENTITYBULKMERGE_H_
#define MONGOC_MONGOCENTITYBULKMERGE_H_

//
// FILE            mongocEntityBulkMerge.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//
#include <stdint.h>                                   // uint64_t

#include "kjson/KjNode.h"                             // KjNode
#include "swNgsild/ldEntityMerge.h"                   // LdMergeReport

#include "db/Tenant.h"                                 // Tenant



// -----------------------------------------------------------------------------
//
// mongocEntityBulkMerge - Batch Merge (§ 5.6.10) for mongoc.
//
// v1: single pool checkout + single collection handle, loops
// mongocEntityMergeOne. That's still 2 round-trips per fragment
// (retrieve + update), but with one TCP connection for the whole
// batch.
//
// Future optimisation (TODO): refactor mongocEntityMergeOne to split
// compute-update-doc from execute-update-doc, and submit all updates
// as one mongoc_bulk_operation_execute.
//
extern int mongocEntityBulkMerge(Tenant* tenantP, KjNode* fragmentsArr,
                                 uint64_t ts, int* resultsV,
                                 LdMergeReport* reportsV,
                                 KjNode** snapshotsV);

#endif  // MONGOC_MONGOCENTITYBULKMERGE_H_
