#ifndef SWRAMDB_RAMDBENTITYCREATE_H_
#define SWRAMDB_RAMDBENTITYCREATE_H_

//
// FILE            ramdbEntityCreate.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//
#include "kjson/KjNode.h"                            // KjNode

#include "db/Tenant.h"                                 // Tenant



// -----------------------------------------------------------------------------
//
// ramdbEntityCreate -
//
extern int ramdbEntityCreate(Tenant* tenantP, const char* entityId, KjNode* entityP);

#endif  // SWRAMDB_RAMDBENTITYCREATE_H_
