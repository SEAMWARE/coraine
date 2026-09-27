#ifndef CORDB_CORDBREGISTRATIONRETRIEVE_H_
#define CORDB_CORDBREGISTRATIONRETRIEVE_H_

//
// FILE            corDbRegistrationRetrieve.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
#include "corTree/CorNode.h"                         // CorNode

#include "db/Tenant.h"                               // Tenant

extern int corDbRegistrationRetrieve(Tenant* tenantP, const char* regId, CorNode** regPP);

#endif  // CORDB_CORDBREGISTRATIONRETRIEVE_H_