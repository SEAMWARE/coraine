#ifndef SWRAMDB_RAMDBREGISTRATIONDELETE_H_
#define SWRAMDB_RAMDBREGISTRATIONDELETE_H_

//
// FILE            ramdbRegistrationDelete.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//
#include "db/Tenant.h"                               // Tenant

extern int ramdbRegistrationDelete(Tenant* tenantP, const char* regId);

#endif  // SWRAMDB_RAMDBREGISTRATIONDELETE_H_
