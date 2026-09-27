#ifndef MONGOC_MONGOCSUBSCRIPTIONQUERY_H_
#define MONGOC_MONGOCSUBSCRIPTIONQUERY_H_

//
// FILE            mongocSubscriptionQuery.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//

#include "db/Tenant.h"                               // Tenant
#include "corTree/CorNode.h"                         // CorNode

extern int mongocSubscriptionQuery(Tenant* tenantP, int limit, int offset, CorNode** arrayPP);

#endif  // MONGOC_MONGOCSUBSCRIPTIONQUERY_H_
