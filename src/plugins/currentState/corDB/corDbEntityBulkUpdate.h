#ifndef CORDB_CORDBENTITYBULKUPDATE_H_
#define CORDB_CORDBENTITYBULKUPDATE_H_

//
// FILE            corDbEntityBulkUpdate.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
#include "corTree/CorNode.h"                       // CorNode
#include "db/Tenant.h"                             // Tenant



// -----------------------------------------------------------------------------
//
// corDbEntityBulkUpdate - replace N existing entities with the caller's
// already-merged final states. resultsV[i] ∈ {DB_OK, DB_NOT_FOUND, DB_ERR}.
//
extern int corDbEntityBulkUpdate(Tenant* tenantP, CorNode* entitiesArr, int* resultsV);

#endif
