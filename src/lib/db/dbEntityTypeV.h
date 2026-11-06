#ifndef DB_DBENTITYTYPEV_H_
#define DB_DBENTITYTYPEV_H_

//
// FILE            dbEntityTypeV.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
#include "db/Tenant.h"                                // Tenant



// -----------------------------------------------------------------------------
//
// dbEntityTypeV - the types of the local copy of an entity, NULL-terminated, NULL when there is none
//
extern char** dbEntityTypeV(Tenant* tenantP, const char* entityId);

#endif  // DB_DBENTITYTYPEV_H_
