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
#include "kjson/KjNode.h"                            // KjNode

#include "db/Tenant.h"                               // Tenant



// -----------------------------------------------------------------------------
//
// corDbSubscriptionCreate -
//
extern int corDbSubscriptionCreate(Tenant* tenantP, const char* subId, KjNode* subP);

#endif  // CORDB_CORDBSUBSCRIPTIONCREATE_H_