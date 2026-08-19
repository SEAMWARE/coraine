//
// FILE            regConflictCheck.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
// § 5.9.2 / § 12.2.3.4 — registration mode-conflict checks, shared by the
// Create (POST) and Update (PATCH) paths.
//
#include <string.h>                                  // strcmp

#include "kjson/kjLookup.h"                          // kjLookup
#include "kjson/KjNode.h"                            // KjNode
#include "kalloc/KAlloc.h"                           // KAlloc
#include "kalloc/kaAlloc.h"                          // kaAlloc
#include "kbase/kStringInArray.h"                    // kStringInArray
#include "corJsonld/corLdExpand.h"                     // corLdExpand, corLdAlreadyExpanded
#include "corNgsild/corNgsild.h"                       // ldError, LD_ERROR_*, corNgsild
#include "corNgsild/LdVocab.h"                        // LD_VOCAB_*
#include "corNgsild/LdRegCache.h"                     // LdRegCache, LdRegCacheItem, LdRegMode
#include "corNgsild/ldDistOp.h"                       // ldDistOpEndpointIsSelf

#include "db/DbDriver.h"                             // db, DB_OK
#include "db/Tenant.h"                               // Tenant

#include "serviceRoutines/regConflictCheck.h"        // Own interface



// -----------------------------------------------------------------------------
//
// attrSetsOverlap - do two RegistrationInfo attr-sets share any attribute?
//
// An info element with no attributeNames means "all attributes" (per spec
// § 5.2.10 the absence of attribute restrictions means the registration covers
// any attribute). Wildcard always overlaps.
//
static bool attrSetsOverlap(char** attrsA, char** attrsB)
{
  bool aWild = (attrsA == NULL);
  bool bWild = (attrsB == NULL);

  if (aWild || bWild)
    return true;

  for (int i = 0; attrsA[i] != NULL; i++)
    if (kStringInArray(attrsA[i], attrsB)) return true;

  return false;
}



// -----------------------------------------------------------------------------
//
// attrIRIArray - extract an attributeNames array as a NULL-terminated
//                array of EXPANDED IRIs.
//
// corLdExpandTree intentionally doesn't @vocab-coerce array values (would
// launder bad input past validators), so attributeNames items arrive in this
// routine as their short-name form. The cached items are
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
    if (corLdAlreadyExpanded(s) == false)
    {
      char* expanded = corLdExpand(NULL, s, allocP, NULL, NULL);
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
//  new exclusive ← blocked by existing exclusive | redirect
//  new redirect  ← blocked by existing exclusive             (multiple redirects coexist)
//  new inclusive ← (no creation-time block per spec)
//  new auxiliary ← (no creation-time block per spec)
//
// An exclusive reg is ALLOWED to overlap an existing INCLUSIVE one: that collision
// is permitted, the exclusive source's attributes being stripped from the inclusive
// contribution at runtime. Likewise redirect + inclusive coexist. Only exclusive↔redirect
// is a hard conflict (both claim sole authority over the attribute), rejected either way.
//
static bool blockingMode(LdRegMode newMode, LdRegMode cachedMode)
{
  if (newMode == LdRegModeExclusive)
    return (cachedMode == LdRegModeExclusive ||
            cachedMode == LdRegModeRedirect);

  if (newMode == LdRegModeRedirect)
    return (cachedMode == LdRegModeExclusive);

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
// 'selfRegId' (may be NULL) is skipped: an in-place PATCH must not conflict with
// the pre-update copy of the very registration being written.
//
static const char* cacheConflict(LdRegCache* cacheP,
                                  LdRegMode   newMode,
                                  const char* entityId,
                                  const char* entityType,
                                  char**      newAttrs,
                                  const char* selfRegId)
{
  if (cacheP == NULL)
    return NULL;

  for (LdRegCacheItem* itemP = cacheP->itemList; itemP != NULL; itemP = itemP->next)
  {
    if (selfRegId != NULL && itemP->regId != NULL && strcmp(itemP->regId, selfRegId) == 0)
      continue;

    if (blockingMode(newMode, itemP->mode) == false)
      continue;

    for (LdRegInfo* riP = itemP->infoV; riP != NULL; riP = riP->next)
    {
      // Does this RegistrationInfo cover entityId?
      bool entityMatches = false;
      for (LdRegEntityInfo* eiP = riP->entityInfoV; eiP != NULL; eiP = eiP->next)
      {
        // Type must match if specified — a type-Building registration
        // doesn't cover a Vehicle entity. eiP->type == NULL means "any
        // type" (legal per spec but rare).
        if (eiP->type != NULL && entityType != NULL && strcmp(eiP->type, entityType) != 0)
          continue;

        // Spec § 4.3.6.3: exclusive regs have specific entity ids (no idPattern).
        // For the conflict check we only need exact-id intersection — pattern
        // matching is left to the dispatcher.
        if (eiP->id == NULL)
        {
          // EntityInfo with no id (and no pattern relevant for conflict) →
          // covers all entities of its type. Already type-filtered above,
          // so this means "any entity of the matching type" → matches.
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

      if (attrSetsOverlap(newAttrs, riP->attributeNamesV))
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
// of the listed attribute IRIs. A wildcard new-reg attr-set (newAttrs NULL,
// "all attrs") makes this conflict if the entity exists at all.
//
static bool localEntityConflict(Tenant* tenantP, const char* entityId, char** newAttrs)
{
  if (db.entityRetrieve == NULL)
    return false;

  KjNode* entityP = NULL;
  int     r       = db.entityRetrieve(tenantP, entityId, &entityP);
  if (r != DB_OK || entityP == NULL)
    return false;

  // Wildcard new-reg attrs → any entity with this id is a conflict
  if (newAttrs == NULL)
    return true;

  // Walk entity attrs (skipping system fields). The local entity's attr names
  // are already expanded IRIs (post-corLdExpand at create time).
  for (KjNode* attrP = entityP->value.firstChildP; attrP != NULL; attrP = attrP->next)
  {
    if (attrP->name == NULL)
      continue;
    if (strcmp(attrP->name, "id") == 0)   continue;
    if (strcmp(attrP->name, "type") == 0) continue;

    if (kStringInArray(attrP->name, newAttrs)) return true;
  }
  return false;
}



// -----------------------------------------------------------------------------
//
// regModeOf -
//
LdRegMode regModeOf(KjNode* regP)
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
// regConflictCheck -
//
// Auxiliary + inclusive regs: spec defines no creation conflicts; skip.
//
bool regConflictCheck(KjNode* regP, LdRegMode newMode, const char* selfRegId, KAlloc* allocP)
{
  if (newMode != LdRegModeExclusive && newMode != LdRegModeRedirect)
    return false;

  // § 12.2.2.4 / § 12.2.3.4 — a redirect registration says the Entity lives
  // ELSEWHERE, i.e. in another broker, so one that names this broker is a
  // Conflict. The spec words the rule as "endpoint and tenant match", but the
  // tenant is not what makes it wrong: redirecting to ourselves under another
  // tenant still redirects to ourselves, and the data is then not elsewhere at
  // all. So the authority alone decides (spec-doubt #113).
  //
  // Only redirect is restricted. inclusive / exclusive / auxiliary
  // registrations that name this broker stay legal — they are how a single
  // instance federates across its own tenants (see the self-forward path).
  if (newMode == LdRegModeRedirect)
  {
    KjNode* endpointP = kjLookup(regP, "endpoint");

    if ((endpointP != NULL) && (endpointP->type == KjString) && ldDistOpEndpointIsSelf(endpointP->value.s))
    {
      ldError(409, LD_ERROR_CONFLICT, "Conflict",
              "a redirect registration must point at another broker, not at this one ('%s')", endpointP->value.s);
      return true;
    }
  }

  Tenant*       tenantP = (Tenant*) corNgsild.tenantP;
  LdRegCache*   cacheP  = (LdRegCache*) tenantP->regCacheP;

  KjNode* infoArrayP = kjLookup(regP, LD_VOCAB_INFORMATION);
  if (infoArrayP == NULL || infoArrayP->type != KjArray)
    return false;

  for (KjNode* infoP = infoArrayP->value.firstChildP; infoP != NULL; infoP = infoP->next)
  {
    if (infoP->type != KjObject)
      continue;

    char** newAttrs = attrIRIArray(kjLookup(infoP, "attributeNames"), allocP);

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

      // The new reg's EntityInfo MAY carry a type; if it does, the
      // conflict check must respect it — a type-Building existing reg
      // doesn't overlap with a Vehicle entity of any id. NULL = no type
      // constraint on the new side, in which case every cached type
      // counts as matching.
      KjNode*     typeP      = kjLookup(entP, "type");
      const char* entityType = (typeP != NULL && typeP->type == KjString) ? typeP->value.s : NULL;

      // Check 1: cached registration overlap
      const char* conflictingRegId = cacheConflict(cacheP, newMode, entityId, entityType, newAttrs, selfRegId);
      if (conflictingRegId != NULL)
      {
        ldError(409, LD_ERROR_ALREADY_EXISTS, "Conflict",
                "registration overlaps with existing registration '%s' on entity '%s'",
                conflictingRegId, entityId);
        return true;
      }

      // Check 2: local entity overlap
      if (localEntityConflict(tenantP, entityId, newAttrs))
      {
        ldError(409, LD_ERROR_ALREADY_EXISTS, "Conflict",
                "registration overlaps with locally-stored entity '%s'", entityId);
        return true;
      }
    }
  }

  return false;
}
