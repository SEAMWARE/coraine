#ifndef SWRAMDB_RAMDBENTITYRETRIEVE_H_
#define SWRAMDB_RAMDBENTITYRETRIEVE_H_

//
// FILE            ramdbEntityRetrieve.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//
#include "kjson/KjNode.h"                            // KjNode

#include "db/Tenant.h"                                 // Tenant



// -----------------------------------------------------------------------------
//
// ramdbEntityRetrieve -
//
extern int ramdbEntityRetrieve(Tenant* tenantP, const char* entityId, KjNode** entityPP);

#endif  // SWRAMDB_RAMDBENTITYRETRIEVE_H_
