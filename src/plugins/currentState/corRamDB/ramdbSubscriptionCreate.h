#ifndef CORRAMDB_RAMDBSUBSCRIPTIONCREATE_H_
#define CORRAMDB_RAMDBSUBSCRIPTIONCREATE_H_

//
// FILE            ramdbSubscriptionCreate.h
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
// ramdbSubscriptionCreate -
//
extern int ramdbSubscriptionCreate(Tenant* tenantP, const char* subId, KjNode* subP);

#endif  // CORRAMDB_RAMDBSUBSCRIPTIONCREATE_H_
