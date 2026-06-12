#ifndef SWRAMDB_RAMDBSUBSCRIPTIONREPLACE_H_
#define SWRAMDB_RAMDBSUBSCRIPTIONREPLACE_H_

//
// FILE            ramdbSubscriptionReplace.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//

#include "db/Tenant.h"                               // Tenant
#include "kjson/KjNode.h"                            // KjNode

extern int ramdbSubscriptionReplace(Tenant* tenantP, const char* subId, KjNode* subP);

#endif  // SWRAMDB_RAMDBSUBSCRIPTIONREPLACE_H_
