#ifndef MONGOC_MONGOCREGISTRATIONUPDATE_H_
#define MONGOC_MONGOCREGISTRATIONUPDATE_H_

//
// FILE            mongocRegistrationUpdate.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//
#include "kjson/KjNode.h"                            // KjNode

#include "db/Tenant.h"                               // Tenant

extern int mongocRegistrationUpdate(Tenant* tenantP, const char* regId, KjNode* fragmentP);

#endif  // MONGOC_MONGOCREGISTRATIONUPDATE_H_
