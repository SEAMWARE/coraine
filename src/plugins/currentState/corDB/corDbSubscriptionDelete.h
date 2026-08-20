#ifndef CORDB_CORDBSUBSCRIPTIONDELETE_H_
#define CORDB_CORDBSUBSCRIPTIONDELETE_H_

//
// FILE            corDbSubscriptionDelete.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
#include "db/Tenant.h"                               // Tenant



// -----------------------------------------------------------------------------
//
// corDbSubscriptionDelete -
//
extern int corDbSubscriptionDelete(Tenant* tenantP, const char* subId);

#endif  // CORDB_CORDBSUBSCRIPTIONDELETE_H_