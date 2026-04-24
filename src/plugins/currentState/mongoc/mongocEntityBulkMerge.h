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
// Two round-trips for the whole batch:
//   1. one find({_id:{$in:[...]}}) fetches every current document;
//   2. one mongoc_bulk_operation_execute runs all surgical $set/$unset
//      updates computed in memory from the merge reports.
//
// resultsV[i] is one of DB_OK / DB_NOT_FOUND / DB_ERR.
// reportsV[i] receives the per-fragment merge report (ldEntityMerge).
// snapshotsV[i] is populated with the post-merge target tree on DB_OK,
// NULL otherwise.
//
extern int mongocEntityBulkMerge(Tenant* tenantP, KjNode* fragmentsArr,
                                 uint64_t ts, int* resultsV,
                                 LdMergeReport* reportsV,
                                 KjNode** snapshotsV);

#endif  // MONGOC_MONGOCENTITYBULKMERGE_H_
