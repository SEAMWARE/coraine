#ifndef CORRAMDB_RAMDBREGISTRATIONCREATE_H_
#define CORRAMDB_RAMDBREGISTRATIONCREATE_H_

//
// FILE            ramdbRegistrationCreate.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//
#include "kjson/KjNode.h"                            // KjNode

#include "db/Tenant.h"                               // Tenant

extern int ramdbRegistrationCreate(Tenant* tenantP, const char* regId, KjNode* regP);

#endif  // CORRAMDB_RAMDBREGISTRATIONCREATE_H_
