//
// FILE            postCsourceRegistration.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
// POST /ngsi-ld/v1/csourceRegistrations  (NGSI-LD § 5.9.2)
//

#include <string.h>                                  // strlen, strcpy, strcat, strcmp
#include <stdio.h>                                   // snprintf
#include <time.h>                                    // time

#include "corRest/CorRestState.h"                      // corRest
#include "corRest/corRestOutHeader.h"                  // corRestOutHeaderAdd
#include "corTree/corTreeLookup.h"                   // corTreeLookup
#include "corTree/corTreeBuilder.h"                  // corTreeString, corTreeChildAdd
#include "corTree/CorNode.h"                         // CorNode
#include "corAlloc/CorAlloc.h"                       // CorAlloc
#include "corAlloc/corAlloc.h"                       // corAlloc
#include "corNgsild/corNgsild.h"                       // ldError, LD_ERROR_*, corNgsild
#include "corNgsild/ldCheckRegistration.h"            // ldCheckRegistration
#include "corNgsild/LdOp.h"                           // LdOpCreateRegistration
#include "corNgsild/LdRegCache.h"                     // LdRegCache, LdRegCacheItem, LdRegMode
#include "corNgsild/ldRegCache.h"                     // ldRegCacheItemAdd
#include "corNgsild/LdSubCache.h"                     // LdSubCache
#include "corNgsild/ldCsrSubNotify.h"                 // ldCsrSubOnRegCreate
#include "corNgsild/ldDistSub.h"                      // ldDistSubOnRegCreate
#include "corNgsild/ldCsourceAlias.h"                 // ldCsourceAliasForTenant
#include "corNgsild/ldSysTimestamp.h"                 // ldSysTimestampCreate
#include "corNgsild/ldTermId.h"                          // ldNodeRename

#include "db/DbDriver.h"                             // db, DB_OK, DB_ALREADY_EXISTS
#include "db/Tenant.h"                               // Tenant

#include "serviceRoutines/regConflictCheck.h"        // regConflictCheck, regModeOf
#include "serviceRoutines/postCsourceRegistration.h" // Own interface



//
// distSubPersist - persist subordinate mapping after on-reg-create fanout
//
static void distSubPersist(LdSubCacheItem* itemP, void* userData)
{
  if (itemP == NULL || itemP->subId == NULL || db.subscriptionUpdate == NULL)
    return;

  Tenant* tP    = (Tenant*) userData;
  CorNode* fragP = ldDistSubSubordinatesFragment(itemP, corRest.kallocP);
  if (fragP == NULL)
    return;

  db.subscriptionUpdate(tP, itemP->subId, fragP);
}



// -----------------------------------------------------------------------------
//
// regIdGenerate - generate a registration id when none is provided
//
static char* regIdGenerate(CorAlloc* allocP)
{
  static int counter = 0;
  char*      buf     = corAlloc(allocP, 128);

  snprintf(buf, 128, "urn:ngsi-ld:ContextSourceRegistration:%lx:%04x", (long) time(NULL), ++counter & 0xFFFF);

  return buf;
}


// -----------------------------------------------------------------------------
//
// postCsourceRegistration -
//
bool postCsourceRegistration(void)
{
  CorNode* regP = corRest.in.requestTree;

  // Validate the registration
  if (ldCheckRegistration(regP, LdOpCreateRegistration, /*merged*/false, &corRest.kalloc) == false)
    return true;

  // Extract or generate registration id
  CorNode* idP = corTreeLookup(regP, "id");

  if (idP == NULL)
  {
    char* generatedId = regIdGenerate(&corRest.kalloc);

    idP = corTreeString(corRest.kallocP, "id", generatedId);
    corTreeChildAdd(regP, idP);
  }
  else if (idP->type != CorString)
  {
    ldError(400, LD_ERROR_BAD_REQUEST_DATA, "Invalid Field Value", "registration 'id' must be a string");
    return true;
  }
  else if (idP->value.s[0] == 0)
  {
    ldError(400, LD_ERROR_BAD_REQUEST_DATA, "Invalid Field Value", "registration 'id' must not be empty");
    return true;
  }
  else if (ldCheckUri(idP->value.s) == false)
  {
    return true;  // ldCheckUri already raised the ldError
  }

  // § 5.9.2 mode-specific creation conflicts (exclusive / redirect): another
  // overlapping reg in the cache, or a local entity holding the to-be-claimed
  // attrs. inclusive / auxiliary skip these checks per spec. The new reg isn't
  // cached yet, so passing its own id as the self-skip is harmless here.
  if (regConflictCheck(regP, regModeOf(regP), idP->value.s, &corRest.kalloc))
    return true;

  // § 6.4.5 — system-generated createdAt/modifiedAt (nanosecond integers in
  // the persisted tree; rendered to ISO only when the client asks for sysAttrs)
  ldSysTimestampCreate(regP);

  // Create registration in the database
  if (db.registrationCreate == NULL)
  {
    ldError(422, LD_ERROR_OP_NOT_SUPPORTED, "Not Implemented", "registration CRUD not supported by this DB plugin");
    return true;
  }

  int r = db.registrationCreate((Tenant*) corNgsild.tenantP, idP->value.s, regP);

  if (r == DB_ALREADY_EXISTS)
  {
    ldError(409, LD_ERROR_ALREADY_EXISTS, "Already Exists", "registration '%s' already exists", idP->value.s);
    return true;
  }

  if (r != DB_OK)
  {
    ldError(500, LD_ERROR_INTERNAL_ERROR, "Internal Error", "database error creating registration '%s'", idP->value.s);
    return true;
  }

  // Restore "id" key if mongocTreeToBson renamed it to "_id" in-place
  if (idP->name[0] == '_')
    ldNodeRename(idP, "id");

  // Add to per-tenant registration cache. The wrlock serializes against
  // concurrent CSR CRUD + match-path readers, and is held across the fanout so
  // the just-added regItemP can't be freed by a concurrent CSR DELETE.
  Tenant*     tenantP   = (Tenant*) corNgsild.tenantP;
  LdRegCache* regCacheP = (LdRegCache*) tenantP->regCacheP;
  LdRegCacheItem* regItemP = NULL;

  ldRegCacheWrLock(regCacheP);

  if (regCacheP != NULL)
    regItemP = ldRegCacheItemAdd(regCacheP, regP, &corRest.kalloc);

  // § 5.11.7 — fan out "newlyMatching" CsourceNotifications to any CSR-
  // sub whose filter matches this new registration.
  if (regItemP != NULL && tenantP->regSubCacheP != NULL)
    ldCsrSubOnRegCreate((LdSubCache*) tenantP->regSubCacheP, regItemP);

  // § 5.8.1.4 — entity-sub side: scan the entity-sub cache and forward
  // a derived sub to the new CSR for every existing local sub whose
  // filter overlaps. Symmetrical to the create-time fanout that runs
  // when the sub is created BEFORE the CSR.
  if (regItemP != NULL && tenantP->subCacheP != NULL)
  {
    const char* ownAlias = ldCsourceAliasForTenant(tenantP->name, &corRest.kalloc);
    ldDistSubOnRegCreate((LdSubCache*) tenantP->subCacheP, regItemP, ownAlias,
                         distSubPersist, tenantP);
  }

  ldRegCacheUnlock(regCacheP);

  // 201 Created — set Location and Link headers, no body
  corRest.out.httpStatusCode = 201;

  const char* prefix = "/ngsi-ld/v1/csourceRegistrations/";
  int         locLen = strlen(prefix) + strlen(idP->value.s) + 1;
  char*       locBuf = corAlloc(&corRest.kalloc, locLen);

  strcpy(locBuf, prefix);
  strcat(locBuf, idP->value.s);
  corRestOutHeaderAdd("Location", locBuf);

  // § 6.3.6: no Link header on no-body responses.

  return true;
}
