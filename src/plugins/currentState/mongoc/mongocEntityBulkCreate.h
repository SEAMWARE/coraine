#ifndef MONGOC_ENTITY_BULK_CREATE_H
#define MONGOC_ENTITY_BULK_CREATE_H

//
// FILE            mongocEntityBulkCreate.h
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
// mongocEntityBulkCreate - see DbEntityBulkCreateFunc.
//
extern int mongocEntityBulkCreate(Tenant* tenantP, KjNode* entitiesArr, int* resultsV);

#endif  // MONGOC_ENTITY_BULK_CREATE_H
