#ifndef MONGOC_MONGOCREGISTRATIONDELETE_H_
#define MONGOC_MONGOCREGISTRATIONDELETE_H_

//
// FILE            mongocRegistrationDelete.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
#include "db/Tenant.h"                               // Tenant

extern int mongocRegistrationDelete(Tenant* tenantP, const char* regId);

#endif  // MONGOC_MONGOCREGISTRATIONDELETE_H_
