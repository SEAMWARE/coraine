#ifndef MONGOC_MONGOCREGISTRATIONQUERY_H_
#define MONGOC_MONGOCREGISTRATIONQUERY_H_

//
// FILE            mongocRegistrationQuery.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//
#include "kjson/KjNode.h"                            // KjNode

#include "db/Tenant.h"                               // Tenant

extern int mongocRegistrationQuery(Tenant* tenantP, int limit, int offset, KjNode** arrayPP);

#endif  // MONGOC_MONGOCREGISTRATIONQUERY_H_
