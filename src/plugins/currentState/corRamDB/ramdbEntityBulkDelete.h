#ifndef CORRAMDB_RAMDBENTITYBULKDELETE_H_
#define CORRAMDB_RAMDBENTITYBULKDELETE_H_

//
// FILE            ramdbEntityBulkDelete.h
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
// ramdbEntityBulkDelete - Batch Delete (§ 5.6.11) for corRamDB.
//
// Walks idV, for each id: finds the stored entity, clones it into the
// request arena for snapshotsV, removes from the store, sets
// resultsV[i] = DB_OK. Missing ids get DB_NOT_FOUND.
//
extern int ramdbEntityBulkDelete(Tenant* tenantP, const char** idV, int N,
                                 int* resultsV, KjNode** snapshotsV);

#endif  // CORRAMDB_RAMDBENTITYBULKDELETE_H_
