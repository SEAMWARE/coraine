#ifndef CORRAMDB_RAMDBSUBSCRIPTIONQUERY_H_
#define CORRAMDB_RAMDBSUBSCRIPTIONQUERY_H_

//
// FILE            ramdbSubscriptionQuery.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
#include "kjson/KjNode.h"                            // KjNode

#include "db/Tenant.h"                               // Tenant



// -----------------------------------------------------------------------------
//
// ramdbSubscriptionQuery -
//
extern int ramdbSubscriptionQuery(Tenant* tenantP, int limit, int offset, KjNode** arrayPP);

#endif  // CORRAMDB_RAMDBSUBSCRIPTIONQUERY_H_
