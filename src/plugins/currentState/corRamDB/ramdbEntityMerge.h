#ifndef CORRAMDB_RAMDBENTITYMERGE_H_
#define CORRAMDB_RAMDBENTITYMERGE_H_

//
// FILE            ramdbEntityMerge.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
#include "kjson/KjNode.h"                             // KjNode
#include "corNgsild/ldEntityMerge.h"                   // LdMergeReport

#include "db/Tenant.h"                                 // Tenant



// -----------------------------------------------------------------------------
//
// ramdbApplyReportToLive - apply a merge report to a live stored entity,
// copying changed attributes (and refreshed modifiedAt/type/scope) from the
// already-merged `merged` tree. Shared by the single-entity and batch paths.
//
extern void ramdbApplyReportToLive(KjNode* live, KjNode* merged, LdMergeReport* reportP);



// -----------------------------------------------------------------------------
//
// ramdbEntityChangesApply - persist a merged single entity (DB driver entry).
//
// The broker has already merged `mergedEntity` (a request-arena clone) and
// produced `reportP`; this applies the change report to the live stored tree.
//
extern int ramdbEntityChangesApply(Tenant* tenantP, const char* entityId,
                                   KjNode* mergedEntity, LdMergeReport* reportP);

#endif  // CORRAMDB_RAMDBENTITYMERGE_H_
