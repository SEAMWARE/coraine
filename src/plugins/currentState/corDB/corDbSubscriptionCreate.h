#ifndef CORDB_CORDBSUBSCRIPTIONCREATE_H_
#define CORDB_CORDBSUBSCRIPTIONCREATE_H_

//
// FILE            corDbSubscriptionCreate.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
#include "corTree/CorNode.h"                         // CorNode

#include "db/Tenant.h"                               // Tenant



// -----------------------------------------------------------------------------
//
// corDbSubscriptionCreate -
//
extern int corDbSubscriptionCreate(Tenant* tenantP, const char* subId, CorNode* subP);

#endif  // CORDB_CORDBSUBSCRIPTIONCREATE_H_