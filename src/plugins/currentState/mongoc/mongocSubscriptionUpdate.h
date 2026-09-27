#ifndef MONGOC_MONGOCSUBSCRIPTIONUPDATE_H_
#define MONGOC_MONGOCSUBSCRIPTIONUPDATE_H_

//
// FILE            mongocSubscriptionUpdate.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//

#include "db/Tenant.h"                               // Tenant
#include "corTree/CorNode.h"                         // CorNode

extern int mongocSubscriptionUpdate(Tenant* tenantP, const char* subId, CorNode* fragmentP);

#endif  // MONGOC_MONGOCSUBSCRIPTIONUPDATE_H_
