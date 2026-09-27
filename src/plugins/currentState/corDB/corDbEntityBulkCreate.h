#ifndef CORDB_CORDBENTITYBULKCREATE_H_
#define CORDB_CORDBENTITYBULKCREATE_H_

//
// FILE            corDbEntityBulkCreate.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
#include "corTree/CorNode.h"                       // CorNode
#include "db/DbDriver.h"                           // Tenant



// -----------------------------------------------------------------------------
//
// corDbEntityBulkCreate -
//
extern int corDbEntityBulkCreate(Tenant* tenantP, CorNode* entitiesArr, int* resultsV);

#endif  // CORDB_CORDBENTITYBULKCREATE_H_