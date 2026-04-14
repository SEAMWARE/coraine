#ifndef SWRAMDB_RAMDBSUBSCRIPTIONCREATE_H_
#define SWRAMDB_RAMDBSUBSCRIPTIONCREATE_H_

//
// FILE            ramdbSubscriptionCreate.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//
#include "kjson/KjNode.h"                            // KjNode

#include "db/Tenant.h"                               // Tenant



// -----------------------------------------------------------------------------
//
// ramdbSubscriptionCreate -
//
extern int ramdbSubscriptionCreate(Tenant* tenantP, const char* subId, KjNode* subP);

#endif  // SWRAMDB_RAMDBSUBSCRIPTIONCREATE_H_
