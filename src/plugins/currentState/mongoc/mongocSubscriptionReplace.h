#ifndef MONGOC_MONGOCSUBSCRIPTIONREPLACE_H_
#define MONGOC_MONGOCSUBSCRIPTIONREPLACE_H_

//
// FILE            mongocSubscriptionReplace.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//

#include "db/Tenant.h"                               // Tenant
#include "kjson/KjNode.h"                            // KjNode

extern int mongocSubscriptionReplace(Tenant* tenantP, const char* subId, KjNode* subP);

#endif  // MONGOC_MONGOCSUBSCRIPTIONREPLACE_H_
