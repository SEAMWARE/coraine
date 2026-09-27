#ifndef CORAINE_CURRENTSTATE_MONGOC_ENTITY_BULK_UPDATE_H_
#define CORAINE_CURRENTSTATE_MONGOC_ENTITY_BULK_UPDATE_H_

//
// FILE            mongocEntityBulkUpdate.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
#include "corTree/CorNode.h"                           // CorNode
#include "db/Tenant.h"                                 // Tenant



// -----------------------------------------------------------------------------
//
// mongocEntityBulkUpdate - replace N existing entities with the caller's
// already-merged final states, in one bulk round-trip.
//
// resultsV[i] ∈ {DB_OK, DB_NOT_FOUND, DB_ERR}.
//
extern int mongocEntityBulkUpdate(Tenant* tenantP, CorNode* entitiesArr, int* resultsV);

#endif
