#ifndef MONGOC_MONGOCSUBSCRIPTIONRETRIEVE_H_
#define MONGOC_MONGOCSUBSCRIPTIONRETRIEVE_H_

//
// FILE            mongocSubscriptionRetrieve.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//

#include "db/Tenant.h"                               // Tenant
#include "kjson/KjNode.h"                            // KjNode

extern int mongocSubscriptionRetrieve(Tenant* tenantP, const char* subId, KjNode** subPP);

#endif  // MONGOC_MONGOCSUBSCRIPTIONRETRIEVE_H_
