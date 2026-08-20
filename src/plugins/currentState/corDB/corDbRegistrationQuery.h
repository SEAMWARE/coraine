#ifndef CORDB_CORDBREGISTRATIONQUERY_H_
#define CORDB_CORDBREGISTRATIONQUERY_H_

//
// FILE            corDbRegistrationQuery.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
#include "kjson/KjNode.h"                            // KjNode

#include "db/Tenant.h"                               // Tenant

extern int corDbRegistrationQuery(Tenant* tenantP, int limit, int offset, KjNode** arrayPP);

#endif  // CORDB_CORDBREGISTRATIONQUERY_H_