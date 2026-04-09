#ifndef MONGOC_MONGOCENTITYCREATE_H_
#define MONGOC_MONGOCENTITYCREATE_H_

//
// FILE            mongocEntityCreate.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//

#include "kjson/KjNode.h"                            // KjNode

#include "db/Tenant.h"                                 // Tenant



// -----------------------------------------------------------------------------
//
// mongocEntityCreate -
//
extern int mongocEntityCreate(Tenant* tenantP, const char* entityId, KjNode* entityP);

#endif  // MONGOC_MONGOCENTITYCREATE_H_
