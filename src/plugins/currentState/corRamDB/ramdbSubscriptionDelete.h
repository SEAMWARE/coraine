#ifndef CORRAMDB_RAMDBSUBSCRIPTIONDELETE_H_
#define CORRAMDB_RAMDBSUBSCRIPTIONDELETE_H_

//
// FILE            ramdbSubscriptionDelete.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
#include "db/Tenant.h"                               // Tenant



// -----------------------------------------------------------------------------
//
// ramdbSubscriptionDelete -
//
extern int ramdbSubscriptionDelete(Tenant* tenantP, const char* subId);

#endif  // CORRAMDB_RAMDBSUBSCRIPTIONDELETE_H_
