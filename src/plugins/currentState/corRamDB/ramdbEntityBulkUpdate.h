#ifndef CORAINE_CURRENTSTATE_RAMDB_ENTITY_BULK_UPDATE_H_
#define CORAINE_CURRENTSTATE_RAMDB_ENTITY_BULK_UPDATE_H_

//
// FILE            ramdbEntityBulkUpdate.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//
#include "kjson/KjNode.h"                          // KjNode
#include "db/Tenant.h"                             // Tenant



// -----------------------------------------------------------------------------
//
// ramdbEntityBulkUpdate - replace N existing entities with the caller's
// already-merged final states. resultsV[i] ∈ {DB_OK, DB_NOT_FOUND, DB_ERR}.
//
extern int ramdbEntityBulkUpdate(Tenant* tenantP, KjNode* entitiesArr, int* resultsV);

#endif
