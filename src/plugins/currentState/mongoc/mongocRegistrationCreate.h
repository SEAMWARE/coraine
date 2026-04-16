#ifndef MONGOC_MONGOCREGISTRATIONCREATE_H_
#define MONGOC_MONGOCREGISTRATIONCREATE_H_

//
// FILE            mongocRegistrationCreate.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//
#include "kjson/KjNode.h"                            // KjNode

#include "db/Tenant.h"                               // Tenant

extern int mongocRegistrationCreate(Tenant* tenantP, const char* regId, KjNode* regP);

#endif  // MONGOC_MONGOCREGISTRATIONCREATE_H_
