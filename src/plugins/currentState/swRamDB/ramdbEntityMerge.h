#ifndef SWRAMDB_RAMDBENTITYMERGE_H_
#define SWRAMDB_RAMDBENTITYMERGE_H_

//
// FILE            ramdbEntityMerge.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//
#include <stdint.h>                                   // uint64_t

#include "kjson/KjNode.h"                             // KjNode
#include "swNgsild/ldEntityMerge.h"                   // LdMergeReport

#include "db/Tenant.h"                                 // Tenant



// -----------------------------------------------------------------------------
//
// ramdbEntityMergeOne - apply Merge Entity to a single stored entity.
//
// entitiesP:  the tenant's "entities" KjArray (as returned by ramdbEntities())
// entityId:   id of the entity to merge into
// fragmentDb: the patch fragment in DB-model, expanded form
// ts:         modifiedAt timestamp (epoch nanoseconds)
// reportP:    out-param for per-attribute change records
//
// Returns DB_OK, DB_NOT_FOUND, or DB_ERR. Mutates the live stored tree
// directly — no clone, no replace.
//
extern int ramdbEntityMergeOne(KjNode* entitiesP, const char* entityId,
                               KjNode* fragmentDb, uint64_t ts,
                               LdMergeReport* reportP, bool deepMerge);



// -----------------------------------------------------------------------------
//
// ramdbEntityMerge - DB driver entry point (single entity)
//
extern int ramdbEntityMerge(Tenant* tenantP, const char* entityId,
                            KjNode* fragmentDb, uint64_t ts,
                            LdMergeReport* reportP, bool deepMerge);

#endif  // SWRAMDB_RAMDBENTITYMERGE_H_
