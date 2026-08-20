#ifndef CORDB_CORDBENTITYBULKDELETE_H_
#define CORDB_CORDBENTITYBULKDELETE_H_

//
// FILE            corDbEntityBulkDelete.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
#include "kjson/KjNode.h"                                // KjNode
#include "db/Tenant.h"                                   // Tenant



// -----------------------------------------------------------------------------
//
// corDbEntityBulkDelete - Batch Delete (§ 5.6.11) for corDB.
//
// Walks idV, for each id: finds the stored entity, clones it into the
// request arena for snapshotsV, removes from the store, sets
// resultsV[i] = DB_OK. Missing ids get DB_NOT_FOUND.
//
extern int corDbEntityBulkDelete(Tenant* tenantP, const char** idV, int N,
                                 int* resultsV, KjNode** snapshotsV);

#endif  // CORDB_CORDBENTITYBULKDELETE_H_