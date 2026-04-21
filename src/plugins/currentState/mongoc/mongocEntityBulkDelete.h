#ifndef MONGOC_MONGOCENTITYBULKDELETE_H_
#define MONGOC_MONGOCENTITYBULKDELETE_H_

//
// FILE            mongocEntityBulkDelete.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//
#include "kjson/KjNode.h"                             // KjNode
#include "db/Tenant.h"                                // Tenant



// -----------------------------------------------------------------------------
//
// mongocEntityBulkDelete - Batch Delete (§ 5.6.11) for mongoc.
//
// Two round-trips:
//   1. find({_id: {$in: [ids]}}) — fetch full docs to populate
//      snapshotsV (for pre-delete notifications).
//   2. bulk_operation_execute with delete_one per id that existed.
//
// Ids not returned by the find → resultsV[i] = DB_NOT_FOUND,
// snapshotsV[i] = NULL. Bulk execute failures downgrade per-entry
// DB_OK -> DB_ERR.
//
extern int mongocEntityBulkDelete(Tenant* tenantP, const char** idV, int N,
                                  int* resultsV, KjNode** snapshotsV);

#endif  // MONGOC_MONGOCENTITYBULKDELETE_H_
