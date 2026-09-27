#ifndef CORDB_CORDBSUBSCRIPTIONRETRIEVE_H_
#define CORDB_CORDBSUBSCRIPTIONRETRIEVE_H_

//
// FILE            corDbSubscriptionRetrieve.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
#include "corTree/CorNode.h"                         // CorNode

#include "db/Tenant.h"                               // Tenant



// -----------------------------------------------------------------------------
//
// corDbSubscriptionRetrieve -
//
extern int corDbSubscriptionRetrieve(Tenant* tenantP, const char* subId, CorNode** subPP);

#endif  // CORDB_CORDBSUBSCRIPTIONRETRIEVE_H_