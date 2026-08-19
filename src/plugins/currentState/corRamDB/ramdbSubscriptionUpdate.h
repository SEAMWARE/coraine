#ifndef CORRAMDB_RAMDBSUBSCRIPTIONUPDATE_H_
#define CORRAMDB_RAMDBSUBSCRIPTIONUPDATE_H_

//
// FILE            ramdbSubscriptionUpdate.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//
#include "kjson/KjNode.h"                            // KjNode

#include "db/Tenant.h"                               // Tenant



// -----------------------------------------------------------------------------
//
// ramdbSubscriptionUpdate -
//
extern int ramdbSubscriptionUpdate(Tenant* tenantP, const char* subId, KjNode* fragmentP);

#endif  // CORRAMDB_RAMDBSUBSCRIPTIONUPDATE_H_
