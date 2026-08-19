#ifndef CORRAMDB_RAMDB_ENTITY_ATTRS_SET_H_
#define CORRAMDB_RAMDB_ENTITY_ATTRS_SET_H_

//
// FILE            ramdbEntityAttrsSet.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//
// Thin ramdb wrapper around corNgsild's ldEntityAttrsSet — locates the
// live entity in the tenant store and applies the fragment in place.
//

#include <stdbool.h>                                  // bool
#include <stdint.h>                                   // uint64_t

#include "kjson/KjNode.h"                             // KjNode
#include "corNgsild/ldEntityMerge.h"                   // LdMergeReport

#include "db/Tenant.h"                                 // Tenant



extern int ramdbEntityAttrsSet(Tenant* tenantP, const char* entityId,
                               KjNode* fragmentDb, bool overwriteScope,
                               uint64_t ts, LdMergeReport* reportP);

#endif  // CORRAMDB_RAMDB_ENTITY_ATTRS_SET_H_
