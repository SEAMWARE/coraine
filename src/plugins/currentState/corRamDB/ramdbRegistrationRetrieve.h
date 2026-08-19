#ifndef CORRAMDB_RAMDBREGISTRATIONRETRIEVE_H_
#define CORRAMDB_RAMDBREGISTRATIONRETRIEVE_H_

//
// FILE            ramdbRegistrationRetrieve.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//
#include "kjson/KjNode.h"                            // KjNode

#include "db/Tenant.h"                               // Tenant

extern int ramdbRegistrationRetrieve(Tenant* tenantP, const char* regId, KjNode** regPP);

#endif  // CORRAMDB_RAMDBREGISTRATIONRETRIEVE_H_
