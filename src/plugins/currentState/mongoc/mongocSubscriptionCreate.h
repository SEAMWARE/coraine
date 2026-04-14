#ifndef MONGOC_MONGOCSUBSCRIPTIONCREATE_H_
#define MONGOC_MONGOCSUBSCRIPTIONCREATE_H_

//
// FILE            mongocSubscriptionCreate.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//

#include "db/Tenant.h"                               // Tenant
#include "kjson/KjNode.h"                            // KjNode

extern int mongocSubscriptionCreate(Tenant* tenantP, const char* subId, KjNode* subP);

#endif  // MONGOC_MONGOCSUBSCRIPTIONCREATE_H_
