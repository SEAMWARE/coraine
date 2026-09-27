#ifndef CORDB_CORDBSUBSCRIPTIONREPLACE_H_
#define CORDB_CORDBSUBSCRIPTIONREPLACE_H_

//
// FILE            corDbSubscriptionReplace.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//

#include "db/Tenant.h"                               // Tenant
#include "corTree/CorNode.h"                         // CorNode

extern int corDbSubscriptionReplace(Tenant* tenantP, const char* subId, CorNode* subP);

#endif  // CORDB_CORDBSUBSCRIPTIONREPLACE_H_