//
// FILE            postCsourceRegistration.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//
// POST /ngsi-ld/v1/csourceRegistrations  (NGSI-LD § 5.9.2)
//

#include <string.h>                                  // strlen, strcpy, strcat, strcmp
#include <stdio.h>                                   // snprintf
#include <time.h>                                    // time

#include "swRest/SwRestState.h"                      // swRest
#include "swRest/swRestOutHeader.h"                  // swRestOutHeaderAdd
#include "kjson/kjLookup.h"                          // kjLookup
#include "kjson/kjBuilder.h"                         // kjString, kjChildAdd
#include "kjson/KjNode.h"                            // KjNode
#include "kalloc/KAlloc.h"                           // KAlloc
#include "kalloc/kaAlloc.h"                          // kaAlloc
#include "kbase/kStringInArray.h"                    // kStringInArray
#include "swJsonld/swldInit.h"                       // swldCoreContext
#include "swJsonld/SwldContext.h"                    // SwldContext
#include "swJsonld/swldExpand.h"                     // swldExpand, swldAlreadyExpanded
#include "swNgsild/swNgsild.h"                       // ldError, LD_ERROR_*, swNgsild
#include "swNgsild/ldCheckRegistration.h"            // ldCheckRegistration
#include "swNgsild/LdVocab.h"                        // LD_VOCAB_*
#include "swNgsild/LdOp.h"                           // LdOpCreateRegistration
#include "swNgsild/LdRegCache.h"                     // LdRegCache, LdRegCacheItem, LdRegMode
#include "swNgsild/ldRegCache.h"                     // ldRegCacheItemAdd
#include "swNgsild/LdSubCache.h"                     // LdSubCache
#include "swNgsild/ldCsrSubNotify.h"                 // ldCsrSubOnRegCreate
#include "swNgsild/ldDistSub.h"                      // ldDistSubOnRegCreate
#include "swNgsild/ldCsourceAlias.h"                 // ldCsourceAliasForTenant
#include "swNgsild/SwNgsild.h"                       // ldLocalOnly

#include "db/DbDriver.h"                             // db, DB_OK, DB_ALREADY_EXISTS
#include "db/Tenant.h"                               // Tenant

#include "serviceRoutines/postCsourceRegistration.h" // Own interface



//
// distSubPersist - persist subordinate mapping after on-reg-create fanout
//
static void distSubPersist(LdSubCacheItem* itemP, void* userData)
{
  if (itemP == NULL || itemP->subId == NULL || db.subscriptionUpdate == NULL)
    return;

  Tenant* tP    = (Tenant*) userData;
  KjNode* fragP = ldDistSubSubordinatesFragment(itemP, swRest.kjsonP);
  if (fragP == NULL)
    return;

  db.subscriptionUpdate(tP, itemP->subId, fragP);
}



// -----------------------------------------------------------------------------
//
// regIdGenerate - generate a registration id when none is provided
//
static char* regIdGenerate(KAlloc* allocP)
{
  static int counter = 0;
  char*      buf     = kaAlloc(allocP, 128);

  snprintf(buf, 128, "urn:ngsi-ld:ContextSourceRegistration:%lx:%04x", (long) time(NULL), ++counter & 0xFFFF);

  return buf;
}






// -----------------------------------------------------------------------------
//
// attrSetsOverlap - do two RegistrationInfo attr-sets share any attribute?
//
// An info element with NEITHER propertyNames NOR relationshipNames means
// "all attributes" (per spec § 5.2.10 the absence of attribute restrictions
// means the registration covers any attribute). Wildcard always overlaps.
//
static bool attrSetsOverlap(char** propsA, char** relsA, char** propsB, char** relsB)
{
  bool aWild = (propsA == NULL && relsA == NULL);
  bool bWild = (propsB == NULL && relsB == NULL);

  if (aWild || bWild)
    return true;

  if (propsA != NULL)
  {
    for (int i = 0; propsA[i] != NULL; i++)
    {
      if (kStringInArray(propsA[i], propsB)) return true;
      if (kStringInArray(propsA[i], relsB))  return true;
    }
  }
  if (relsA != NULL)
  {
    for (int i = 0; relsA[i] != NULL; i++)
    {
      if (kStringInArray(relsA[i], propsB)) return true;
      if (kStringInArray(relsA[i], relsB))  return true;
    }
  }
  return false;
}



// -----------------------------------------------------------------------------
//
// attrIRIArray - extract propertyNames / relationshipNames as a NULL-terminated
//                array of EXPANDED IRIs.
//
// swldExpandTree intentionally doesn't @vocab-coerce array values (would
// launder bad input past validators), so propertyNames / relationshipNames
// arrive in this routine as their short-name form. The cached items are
// stored expanded (ldRegCache.c attrIRIArrayExtract) so we expand the new
// reg's names on-the-fly to compare apples-to-apples.
//
static char** attrIRIArray(KjNode* arrP, KAlloc* allocP)
{
  if (arrP == NULL || arrP->type != KjArray)
    return NULL;

  int count = 0;
  for (KjNode* sP = arrP->value.firstChildP; sP != NULL; sP = sP->next)
    if (sP->type == KjString)
      count++;

  if (count == 0)
    return NULL;

  char** v = (char**) kaAlloc(allocP, (count + 1) * sizeof(char*));
  int    ix = 0;
  for (KjNode* sP = arrP->value.firstChildP; sP != NULL; sP = sP->next)
  {
    if (sP->type != KjString)
      continue;
    char* s = sP->value.s;
    if (swldAlreadyExpanded(s) == false)
    {
      char* expanded = swldExpand(NULL, s, allocP, NULL, NULL);
      if (expanded != NULL)
        s = expanded;
    }
    v[ix++] = s;
  }
  v[ix] = NULL;
  return v;
}



// -----------------------------------------------------------------------------
//
// blockingMode - which existing-reg modes block creation of a new reg of mode 'newMode'
//
// Returns true if a cached reg of mode 'cachedMode' would block the new reg.
//
//  new exclusive ← blocked by existing exclusive | redirect | inclusive
//  new redirect  ← blocked by existing exclusive | inclusive  (multiple redirects coexist)
//  new inclusive ← (no creation-time block per spec)
//  new auxiliary ← (no creation-time block per spec)
//
static bool blockingMode(LdRegMode newMode, LdRegMode cachedMode)
{
  if (newMode == LdRegModeExclusive)
    return (cachedMode == LdRegModeExclusive ||
            cachedMode == LdRegModeRedirect  ||
            cachedMode == LdRegModeInclusive);

  if (newMode == LdRegModeRedirect)
    return (cachedMode == LdRegModeExclusive ||
            cachedMode == LdRegModeInclusive);

  return false;
}



// -----------------------------------------------------------------------------
//
// cacheConflict - find a cached reg that conflicts with a new (entityId, attrSet)
//
// For each cached item of a blocking mode, walk its infoV to find one whose
// EntityInfo matches entityId AND whose attr-set overlaps with the new
// (newProps, newRels).  Returns the conflicting item's regId, or NULL.
//
static const char* cacheConflict(LdRegCache* cacheP,
                                  LdRegMode   newMode,
                                  const char* entityId,
                                  char**      newProps,
                                  char**      newRels)
{
  if (cacheP == NULL)
    return NULL;

  for (LdRegCacheItem* itemP = cacheP->itemList; itemP != NULL; itemP = itemP->next)
  {
    if (blockingMode(newMode, itemP->mode) == false)
      continue;

    for (LdRegInfo* riP = itemP->infoV; riP != NULL; riP = riP->next)
    {
      // Does this RegistrationInfo cover entityId?
      bool entityMatches = false;
      for (LdRegEntityInfo* eiP = riP->entityInfoV; eiP != NULL; eiP = eiP->next)
      {
        // Spec § 4.3.6.3: exclusive regs have specific entity ids (no idPattern).
        // For the conflict check we only need exact-id intersection — pattern
        // matching is left to the dispatcher.
        if (eiP->id == NULL)
        {
          // EntityInfo with no id (and no pattern relevant for conflict) →
          // covers all entities of its type. Treat as matching.
          if (eiP->idPatternList == NULL)
          {
            entityMatches = true;
            break;
          }
          // idPattern-only match: out of scope for conflict checks (spec
          // doesn't require it; a pattern match is best-effort).
          continue;
        }

        if (strcmp(eiP->id, entityId) == 0)
        {
          entityMatches = true;
          break;
        }
      }

      if (entityMatches == false)
        continue;

      if (attrSetsOverlap(newProps, newRels, riP->propertyNamesV, riP->relationshipNamesV))
        return itemP->regId;
    }
  }

  return NULL;
}



// -----------------------------------------------------------------------------
//
// localEntityConflict - does local entity 'entityId' have any of the new reg's attrs?
//
// Returns true if the local store has the entity AND it carries at least one
// of the listed attribute IRIs. A wildcard new-reg attr-set (both propsP and
// relsP NULL, "all attrs") makes this conflict if the entity exists at all.
//
static bool localEntityConflict(Tenant* tenantP, const char* entityId, char** newProps, char** newRels)
{
  if (db.entityRetrieve == NULL)
    return false;

  KjNode* entityP = NULL;
  int     r       = db.entityRetrieve(tenantP, entityId, &entityP);
  if (r != DB_OK || entityP == NULL)
    return false;

  // Wildcard new-reg attrs → any entity with this id is a conflict
  if (newProps == NULL && newRels == NULL)
    return true;

  // Walk entity attrs (skipping system fields). The local entity's attr names
  // are already expanded IRIs (post-swldExpand at create time).
  for (KjNode* attrP = entityP->value.firstChildP; attrP != NULL; attrP = attrP->next)
  {
    if (attrP->name == NULL)
      continue;
    if (strcmp(attrP->name, "id") == 0)   continue;
    if (strcmp(attrP->name, "type") == 0) continue;

    if (kStringInArray(attrP->name, newProps)) return true;
    if (kStringInArray(attrP->name, newRels))  return true;
  }
  return false;
}



// -----------------------------------------------------------------------------
//
// conflictCheck - run all creation-time conflict checks for the new reg
//
// Returns true if a conflict was found AND ldError was raised — caller must
// just return true. Returns false if no conflict (caller proceeds with create).
//
// Auxiliary + inclusive new regs: spec defines no creation conflicts; skip.
//
static bool conflictCheck(KjNode* regP, LdRegMode newMode)
{
  if (newMode != LdRegModeExclusive && newMode != LdRegModeRedirect)
    return false;

  Tenant*       tenantP = (Tenant*) swNgsild.tenantP;
  LdRegCache*   cacheP  = (LdRegCache*) tenantP->regCacheP;

  KjNode* infoArrayP = kjLookup(regP, LD_VOCAB_INFORMATION);
  if (infoArrayP == NULL || infoArrayP->type != KjArray)
    return false;

  for (KjNode* infoP = infoArrayP->value.firstChildP; infoP != NULL; infoP = infoP->next)
  {
    if (infoP->type != KjObject)
      continue;

    char** newProps = attrIRIArray(kjLookup(infoP, "propertyNames"),     &swRest.kalloc);
    char** newRels  = attrIRIArray(kjLookup(infoP, "relationshipNames"), &swRest.kalloc);

    KjNode* entitiesP = kjLookup(infoP, LD_VOCAB_ENTITIES);
    if (entitiesP == NULL || entitiesP->type != KjArray)
      continue;

    for (KjNode* entP = entitiesP->value.firstChildP; entP != NULL; entP = entP->next)
    {
      if (entP->type != KjObject)
        continue;

      KjNode* idP = kjLookup(entP, "id");
      if (idP == NULL || idP->type != KjString)
        continue;  // no specific id = nothing to conflict against by id

      const char* entityId = idP->value.s;

      // Check 1: cached registration overlap
      const char* conflictingRegId = cacheConflict(cacheP, newMode, entityId, newProps, newRels);
      if (conflictingRegId != NULL)
      {
        ldError(409, LD_ERROR_ALREADY_EXISTS, "Conflict",
                "registration overlaps with existing registration '%s' on entity '%s'",
                conflictingRegId, entityId);
        return true;
      }

      // Check 2: local entity overlap
      if (localEntityConflict(tenantP, entityId, newProps, newRels))
      {
        ldError(409, LD_ERROR_ALREADY_EXISTS, "Conflict",
                "registration overlaps with locally-stored entity '%s'", entityId);
        return true;
      }
    }
  }

  return false;
}



// -----------------------------------------------------------------------------
//
// modeOf - resolve the new reg's mode (default = inclusive per § 5.2.9)
//
static LdRegMode modeOf(KjNode* regP)
{
  KjNode* modeP = kjLookup(regP, LD_VOCAB_MODE);
  if (modeP == NULL || modeP->type != KjString)
    return LdRegModeInclusive;

  if (strcmp(modeP->value.s, "exclusive") == 0)  return LdRegModeExclusive;
  if (strcmp(modeP->value.s, "redirect")  == 0)  return LdRegModeRedirect;
  if (strcmp(modeP->value.s, "auxiliary") == 0)  return LdRegModeAuxiliary;
  return LdRegModeInclusive;
}



// -----------------------------------------------------------------------------
//
// postCsourceRegistration -
//
bool postCsourceRegistration(void)
{
  KjNode* regP = swRest.in.requestTree;

  // Validate the registration
  if (ldCheckRegistration(regP, LdOpCreateRegistration, &swRest.kalloc) == false)
    return true;

  // Extract or generate registration id
  KjNode* idP = kjLookup(regP, "id");

  if (idP == NULL)
  {
    char* generatedId = regIdGenerate(&swRest.kalloc);

    idP = kjString(NULL, "id", generatedId);
    kjChildAdd(regP, idP);
  }
  else if (idP->type != KjString)
  {
    ldError(400, LD_ERROR_BAD_REQUEST_DATA, "Bad Request", "registration 'id' must be a string");
    return true;
  }
  else if (idP->value.s[0] == 0)
  {
    ldError(400, LD_ERROR_BAD_REQUEST_DATA, "Bad Request", "registration 'id' must not be empty");
    return true;
  }
  else if (ldCheckUri(idP->value.s) == false)
  {
    return true;  // ldCheckUri already raised the ldError
  }

  // § 5.9.2 mode-specific creation conflicts (exclusive / redirect): another
  // overlapping reg in the cache, or a local entity holding the to-be-claimed
  // attrs. inclusive / auxiliary skip these checks per spec.
  if (conflictCheck(regP, modeOf(regP)))
    return true;

  // Create registration in the database
  if (db.registrationCreate == NULL)
  {
    ldError(422, LD_ERROR_OP_NOT_SUPPORTED, "Not Implemented", "registration CRUD not supported by this DB plugin");
    return true;
  }

  int r = db.registrationCreate((Tenant*) swNgsild.tenantP, idP->value.s, regP);

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

  // Restore "id" key if mongocKjTreeToBson renamed it to "_id" in-place
  if (idP->name[0] == '_')
    idP->name = "id";

  // Add to per-tenant registration cache
  Tenant* tenantP = (Tenant*) swNgsild.tenantP;
  LdRegCacheItem* regItemP = NULL;
  if (tenantP->regCacheP != NULL)
    regItemP = ldRegCacheItemAdd((LdRegCache*) tenantP->regCacheP, regP);

  // § 5.11.7 — fan out "newlyMatching" CsourceNotifications to any CSR-
  // sub whose filter matches this new registration.
  if (regItemP != NULL && tenantP->regSubCacheP != NULL)
    ldCsrSubOnRegCreate((LdSubCache*) tenantP->regSubCacheP, regItemP);

  // § 5.8.1.4 — entity-sub side: scan the entity-sub cache and forward
  // a derived sub to the new CSR for every existing local sub whose
  // filter overlaps. Symmetrical to the create-time fanout that runs
  // when the sub is created BEFORE the CSR.
  if (regItemP != NULL && tenantP->subCacheP != NULL && !ldLocalOnly)
  {
    const char* ownAlias = ldCsourceAliasForTenant(tenantP->name, &swRest.kalloc);
    ldDistSubOnRegCreate((LdSubCache*) tenantP->subCacheP, regItemP, ownAlias,
                         distSubPersist, tenantP);
  }

  // 201 Created — set Location and Link headers, no body
  swRest.out.httpStatusCode = 201;

  const char* prefix = "/ngsi-ld/v1/csourceRegistrations/";
  int         locLen = strlen(prefix) + strlen(idP->value.s) + 1;
  char*       locBuf = kaAlloc(&swRest.kalloc, locLen);

  strcpy(locBuf, prefix);
  strcat(locBuf, idP->value.s);
  swRestOutHeaderAdd("Location", locBuf);

  // § 6.3.6: no Link header on no-body responses.

  return true;
}
