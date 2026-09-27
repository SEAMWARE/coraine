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
#include "corTree/CorNode.h"                         // CorNode

#include "db/Tenant.h"                               // Tenant

extern int corDbRegistrationQuery(Tenant* tenantP, int limit, int offset, CorNode** arrayPP);

#endif  // CORDB_CORDBREGISTRATIONQUERY_H_