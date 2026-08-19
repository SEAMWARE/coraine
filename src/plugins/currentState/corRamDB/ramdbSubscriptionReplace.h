#ifndef CORRAMDB_RAMDBSUBSCRIPTIONREPLACE_H_
#define CORRAMDB_RAMDBSUBSCRIPTIONREPLACE_H_

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

#endif  // CORRAMDB_RAMDBSUBSCRIPTIONREPLACE_H_
