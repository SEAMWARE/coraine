#ifndef SWRAMDB_RAMDBREGISTRATIONQUERY_H_
#define SWRAMDB_RAMDBREGISTRATIONQUERY_H_

//
// FILE            ramdbRegistrationQuery.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//
#include "kjson/KjNode.h"                            // KjNode

#include "db/Tenant.h"                               // Tenant

extern int ramdbRegistrationQuery(Tenant* tenantP, int limit, int offset, KjNode** arrayPP);

#endif  // SWRAMDB_RAMDBREGISTRATIONQUERY_H_
