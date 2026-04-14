#ifndef MONGOC_MONGOCSUBSCRIPTIONUPDATE_H_
#define MONGOC_MONGOCSUBSCRIPTIONUPDATE_H_

//
// FILE            mongocSubscriptionUpdate.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//

#include "db/Tenant.h"                               // Tenant
#include "kjson/KjNode.h"                            // KjNode

extern int mongocSubscriptionUpdate(Tenant* tenantP, const char* subId, KjNode* fragmentP);

#endif  // MONGOC_MONGOCSUBSCRIPTIONUPDATE_H_
