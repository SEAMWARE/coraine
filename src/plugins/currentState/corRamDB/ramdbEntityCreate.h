#ifndef CORRAMDB_RAMDBENTITYCREATE_H_
#define CORRAMDB_RAMDBENTITYCREATE_H_

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

#endif  // CORRAMDB_RAMDBENTITYCREATE_H_
