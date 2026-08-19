#ifndef CORRAMDB_RAMDBSUBSCRIPTIONRETRIEVE_H_
#define CORRAMDB_RAMDBSUBSCRIPTIONRETRIEVE_H_

//
// FILE            ramdbSubscriptionRetrieve.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//
#include "kjson/KjNode.h"                            // KjNode

#include "db/Tenant.h"                               // Tenant



// -----------------------------------------------------------------------------
//
// ramdbSubscriptionRetrieve -
//
extern int ramdbSubscriptionRetrieve(Tenant* tenantP, const char* subId, KjNode** subPP);

#endif  // CORRAMDB_RAMDBSUBSCRIPTIONRETRIEVE_H_
