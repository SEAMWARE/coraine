#ifndef CORDB_CORDBSUBSCRIPTIONQUERY_H_
#define CORDB_CORDBSUBSCRIPTIONQUERY_H_

//
// FILE            corDbSubscriptionQuery.h
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
// corDbSubscriptionQuery -
//
extern int corDbSubscriptionQuery(Tenant* tenantP, int limit, int offset, KjNode** arrayPP);

#endif  // CORDB_CORDBSUBSCRIPTIONQUERY_H_