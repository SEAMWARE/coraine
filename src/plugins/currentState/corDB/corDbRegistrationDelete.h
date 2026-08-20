#ifndef CORDB_CORDBREGISTRATIONDELETE_H_
#define CORDB_CORDBREGISTRATIONDELETE_H_

//
// FILE            corDbRegistrationDelete.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
#include "db/Tenant.h"                               // Tenant

extern int corDbRegistrationDelete(Tenant* tenantP, const char* regId);

#endif  // CORDB_CORDBREGISTRATIONDELETE_H_