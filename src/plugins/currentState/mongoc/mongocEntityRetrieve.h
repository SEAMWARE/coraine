#ifndef MONGOC_MONGOCENTITYRETRIEVE_H_
#define MONGOC_MONGOCENTITYRETRIEVE_H_

//
// FILE            mongocEntityRetrieve.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//

#include "kjson/KjNode.h"                            // KjNode

#include "db/Tenant.h"                                 // Tenant



// -----------------------------------------------------------------------------
//
// mongocEntityRetrieve -
//
extern int mongocEntityRetrieve(Tenant* tenantP, const char* entityId, KjNode** entityPP);

#endif  // MONGOC_MONGOCENTITYRETRIEVE_H_
