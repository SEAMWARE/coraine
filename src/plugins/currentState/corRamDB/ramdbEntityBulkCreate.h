#ifndef RAMDB_ENTITY_BULK_CREATE_H
#define RAMDB_ENTITY_BULK_CREATE_H

//
// FILE            ramdbEntityBulkCreate.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
#include "kjson/KjNode.h"                          // KjNode
#include "db/DbDriver.h"                           // Tenant



// -----------------------------------------------------------------------------
//
// ramdbEntityBulkCreate -
//
extern int ramdbEntityBulkCreate(Tenant* tenantP, KjNode* entitiesArr, int* resultsV);

#endif  // RAMDB_ENTITY_BULK_CREATE_H
