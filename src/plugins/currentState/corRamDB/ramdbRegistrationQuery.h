#ifndef CORRAMDB_RAMDBREGISTRATIONQUERY_H_
#define CORRAMDB_RAMDBREGISTRATIONQUERY_H_

//
// FILE            ramdbRegistrationQuery.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
#include "kjson/KjNode.h"                            // KjNode

#include "db/Tenant.h"                               // Tenant

extern int ramdbRegistrationQuery(Tenant* tenantP, int limit, int offset, KjNode** arrayPP);

#endif  // CORRAMDB_RAMDBREGISTRATIONQUERY_H_
