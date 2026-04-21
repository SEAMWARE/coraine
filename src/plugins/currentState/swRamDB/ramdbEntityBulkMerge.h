#ifndef SWRAMDB_RAMDBENTITYBULKMERGE_H_
#define SWRAMDB_RAMDBENTITYBULKMERGE_H_

//
// FILE            ramdbEntityBulkMerge.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//
#include <stdint.h>                                     // uint64_t

#include "kjson/KjNode.h"                               // KjNode
#include "swNgsild/ldEntityMerge.h"                     // LdMergeReport

#include "db/Tenant.h"                                  // Tenant



// -----------------------------------------------------------------------------
//
// ramdbEntityBulkMerge - Batch Merge (§ 5.6.10) for swRamDB.
//
// Loops ramdbEntityMergeOne. resultsV and reportsV are parallel arrays
// of length = number of fragments; each carries the per-entity outcome.
//
extern int ramdbEntityBulkMerge(Tenant* tenantP, KjNode* fragmentsArr,
                                uint64_t ts, int* resultsV,
                                LdMergeReport* reportsV,
                                KjNode** snapshotsV);

#endif  // SWRAMDB_RAMDBENTITYBULKMERGE_H_
