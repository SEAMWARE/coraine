#ifndef CORRAMDB_RAMDBREGISTRATIONUPDATE_H_
#define CORRAMDB_RAMDBREGISTRATIONUPDATE_H_

//
// FILE            ramdbRegistrationUpdate.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//
#include "kjson/KjNode.h"                            // KjNode

#include "db/Tenant.h"                               // Tenant

extern int ramdbRegistrationUpdate(Tenant* tenantP, const char* regId, KjNode* fragmentP);

#endif  // CORRAMDB_RAMDBREGISTRATIONUPDATE_H_
