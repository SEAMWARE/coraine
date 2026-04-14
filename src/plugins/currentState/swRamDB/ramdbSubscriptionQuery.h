#ifndef SWRAMDB_RAMDBSUBSCRIPTIONQUERY_H_
#define SWRAMDB_RAMDBSUBSCRIPTIONQUERY_H_

//
// FILE            ramdbSubscriptionQuery.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//
#include "kjson/KjNode.h"                            // KjNode

#include "db/Tenant.h"                               // Tenant



// -----------------------------------------------------------------------------
//
// ramdbSubscriptionQuery -
//
extern int ramdbSubscriptionQuery(Tenant* tenantP, int limit, int offset, KjNode** arrayPP);

#endif  // SWRAMDB_RAMDBSUBSCRIPTIONQUERY_H_
