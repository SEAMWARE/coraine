#ifndef MONGOC_MONGOCSUBSCRIPTIONQUERY_H_
#define MONGOC_MONGOCSUBSCRIPTIONQUERY_H_

//
// FILE            mongocSubscriptionQuery.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//

#include "db/Tenant.h"                               // Tenant
#include "kjson/KjNode.h"                            // KjNode

extern int mongocSubscriptionQuery(Tenant* tenantP, int limit, int offset, KjNode** arrayPP);

#endif  // MONGOC_MONGOCSUBSCRIPTIONQUERY_H_
