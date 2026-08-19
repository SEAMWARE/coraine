#ifndef MONGOC_MONGOCREGISTRATIONRETRIEVE_H_
#define MONGOC_MONGOCREGISTRATIONRETRIEVE_H_

//
// FILE            mongocRegistrationRetrieve.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
#include "kjson/KjNode.h"                            // KjNode

#include "db/Tenant.h"                               // Tenant

extern int mongocRegistrationRetrieve(Tenant* tenantP, const char* regId, KjNode** regPP);

#endif  // MONGOC_MONGOCREGISTRATIONRETRIEVE_H_
