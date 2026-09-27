#ifndef CORDB_CORDBSUBSCRIPTIONUPDATE_H_
#define CORDB_CORDBSUBSCRIPTIONUPDATE_H_

//
// FILE            corDbSubscriptionUpdate.h
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
// corDbSubscriptionUpdate -
//
extern int corDbSubscriptionUpdate(Tenant* tenantP, const char* subId, CorNode* fragmentP);

#endif  // CORDB_CORDBSUBSCRIPTIONUPDATE_H_