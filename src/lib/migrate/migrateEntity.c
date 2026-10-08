//
// FILE            migrateEntity.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
#include <string.h>                                       // strcmp
#include <stdint.h>                                       // int64_t

#include "corAlloc/corAlloc.h"                            // corAlloc
#include "corTree/CorNode.h"                              // CorNode
#include "corTree/corTreeLookup.h"                        // corTreeLookup
#include "corTree/corTreeBuilder.h"                       // corTreeChildRemove
#include "corRest/CorRestState.h"                         // corRest
#include "corJsonld/corLdExpand.h"                        // KJF_ATTR_TERM
#include "corJsonld/corLdExpandTree.h"                    // corLdExpandEntityTree
#include "corJsonld/corLdInit.h"                          // corLdCoreContext

#include "corNgsild/LdOp.h"                               // LdOpCreateEntity
#include "corNgsild/LdVocab.h"                            // LD_VOCAB_*
#include "corNgsild/LdNormalizeInput.h"                   // ldNormalizeInput
#include "corNgsild/ldCheckEntity.h"                      // ldCheckEntity
#include "corNgsild/ldApiEntityToDbModel.h"               // ldApiEntityToDbModel
#include "corNgsild/ldIsEntityKeyword.h"                  // ldIsEntityMember
#include "corNgsild/ldTermId.h"                           // ldTermId, CorTerm*

#include "db/DbDriver.h"                                  // db, DB_*
#include "db/Tenant.h"                                    // Tenant

#include "migrate/MigrateState.h"                         // MigrateState
#include "migrate/migrateUtil.h"                          // migrateTimeTake, migrateEntityTermsCheck, ...
#include "migrate/migrateEntity.h"                        // Own interface



// -----------------------------------------------------------------------------
//
// SysTimes - the createdAt/modifiedAt of one Attribute instance or Sub-Attribute, as the source had them
//
// Keyed by NAME (attribute, datasetId, sub-attribute) and not by node: what the broker's own
// conversion does to the tree in between (normalization, the dataset wrapping) is its business,
// and the names are what survive it.
//
typedef struct SysTimes
{
  const char*       attrName;
  const char*       datasetId;   // "@none" for the default instance
  const char*       subAttrName; // NULL for the instance itself
  int64_t           createdAt;
  int64_t           modifiedAt;
  struct SysTimes*  next;
} SysTimes;



// -----------------------------------------------------------------------------
//
// timesAdd -
//
static void timesAdd(SysTimes** listPP, const char* attrName, const char* datasetId, const char* subAttrName, int64_t c, int64_t m)
{
  if ((c <= 0) && (m <= 0))
    return;

  SysTimes* stP = (SysTimes*) corAlloc(&corRest.kalloc, sizeof(SysTimes));

  if (m <= 0) m = c;
  if (c <= 0) c = m;

  stP->attrName    = attrName;
  stP->datasetId   = datasetId;
  stP->subAttrName = subAttrName;
  stP->createdAt   = c;
  stP->modifiedAt  = m;
  stP->next        = *listPP;
  *listPP          = stP;
}



// -----------------------------------------------------------------------------
//
// timesFind -
//
static SysTimes* timesFind(SysTimes* listP, const char* attrName, const char* datasetId, const char* subAttrName)
{
  for (SysTimes* stP = listP; stP != NULL; stP = stP->next)
  {
    if (strcmp(stP->attrName, attrName)   != 0) continue;
    if (strcmp(stP->datasetId, datasetId) != 0) continue;

    if ((subAttrName == NULL) != (stP->subAttrName == NULL)) continue;
    if ((subAttrName != NULL) && (strcmp(stP->subAttrName, subAttrName) != 0)) continue;

    return stP;
  }

  return NULL;
}



// -----------------------------------------------------------------------------
//
// timesStrip - drop createdAt/modifiedAt below the levels that keep them (a Sub-Attribute's own)
//
static void timesStrip(CorNode* objP)
{
  if ((objP == NULL) || (objP->type != CorObject))
    return;

  migrateTimeTake(objP, CorTermCreatedAt,  LD_VOCAB_CREATED_AT);
  migrateTimeTake(objP, CorTermModifiedAt, LD_VOCAB_MODIFIED_AT);

  for (CorNode* memberP = objP->value.head; memberP != NULL; memberP = memberP->next)
  {
    if ((memberP->type == CorObject) && ((memberP->flags & KJF_ATTR_TERM) == 0))
      timesStrip(memberP);
  }
}



// -----------------------------------------------------------------------------
//
// instanceTimesTake - take the timestamps of an instance and of its Sub-Attributes
//
static bool instanceTimesTake(SysTimes** listPP, const char* attrName, CorNode* instP)
{
  if (instP->type != CorObject)
    return true;

  CorNode*    dsP       = corTreeLookup(instP, LD_VOCAB_DATASET_ID);
  const char* datasetId = ((dsP != NULL) && (dsP->type == CorString)) ? dsP->value.s : "@none";
  int64_t     c         = migrateTimeTake(instP, CorTermCreatedAt,  LD_VOCAB_CREATED_AT);
  int64_t     m         = migrateTimeTake(instP, CorTermModifiedAt, LD_VOCAB_MODIFIED_AT);

  if ((c < 0) || (m < 0))
    return false;

  timesAdd(listPP, attrName, datasetId, NULL, c, m);

  for (CorNode* subP = instP->value.head; subP != NULL; subP = subP->next)
  {
    if ((subP->type != CorObject) || ((subP->flags & KJF_ATTR_TERM) != 0) || (subP->name == NULL))
      continue;

    int64_t sc = migrateTimeTake(subP, CorTermCreatedAt,  LD_VOCAB_CREATED_AT);
    int64_t sm = migrateTimeTake(subP, CorTermModifiedAt, LD_VOCAB_MODIFIED_AT);

    if ((sc < 0) || (sm < 0))
      return false;

    timesAdd(listPP, attrName, datasetId, subP->name, sc, sm);

    for (CorNode* subSubP = subP->value.head; subSubP != NULL; subSubP = subSubP->next)
    {
      if ((subSubP->type == CorObject) && ((subSubP->flags & KJF_ATTR_TERM) == 0))
        timesStrip(subSubP);
    }
  }

  return true;
}



// -----------------------------------------------------------------------------
//
// timesSet - overwrite the createdAt/modifiedAt the conversion stamped
//
static void timesSet(CorNode* objP, int64_t c, int64_t m)
{
  CorNode* cP = corTreeLookup(objP, LD_VOCAB_CREATED_AT);
  CorNode* mP = corTreeLookup(objP, LD_VOCAB_MODIFIED_AT);

  if ((cP != NULL) && (cP->type == CorInt)) cP->value.i = (long long) c;
  if ((mP != NULL) && (mP->type == CorInt)) mP->value.i = (long long) m;
}



// -----------------------------------------------------------------------------
//
// timesRestore - put the source's timestamps back, on the DB model
//
// The DB model is { attrName: { datasetId: instance } }, every instance and Sub-Attribute
// stamped by ldApiEntityToDbModel with the conversion's clock - the entity's own modifiedAt, set
// as corRest.requestStartTime by the caller. What the source did not have keeps that.
//
static void timesRestore(CorNode* entityP, SysTimes* listP)
{
  for (CorNode* wrapperP = entityP->value.head; wrapperP != NULL; wrapperP = wrapperP->next)
  {
    if ((wrapperP->type != CorObject) || (ldIsEntityMember(wrapperP) == true) || (wrapperP->name == NULL))
      continue;

    for (CorNode* instP = wrapperP->value.head; instP != NULL; instP = instP->next)
    {
      if ((instP->type != CorObject) || (instP->name == NULL))
        continue;

      SysTimes* instTimesP = timesFind(listP, wrapperP->name, instP->name, NULL);

      if (instTimesP != NULL)
        timesSet(instP, instTimesP->createdAt, instTimesP->modifiedAt);

      for (CorNode* subP = instP->value.head; subP != NULL; subP = subP->next)
      {
        if ((subP->type != CorObject) || (subP->name == NULL) || (corTreeLookup(subP, LD_VOCAB_CREATED_AT) == NULL))
          continue;

        SysTimes* subTimesP = timesFind(listP, wrapperP->name, instP->name, subP->name);

        if (subTimesP != NULL)
          timesSet(subP, subTimesP->createdAt, subTimesP->modifiedAt);
        else if (instTimesP != NULL)
          timesSet(subP, instTimesP->createdAt, instTimesP->modifiedAt);
      }
    }
  }
}



// -----------------------------------------------------------------------------
//
// migrateEntityToDbModel - an Entity of the stream, as the broker stores it
//
// The broker's own conversion, in the order a create request goes through it - expand, normalize,
// check, ldApiEntityToDbModel - with one difference: the system timestamps are taken out before
// and put back after, so they are the source's and not the clock's.
//
// On success the tree is the DB model and *createdAtP / *modifiedAtP are the entity's own.
//
bool migrateEntityToDbModel(MigrateState* msP, MigrateKind kind, CorNode* entityP, int64_t* createdAtP, int64_t* modifiedAtP)
{
  if ((entityP == NULL) || (entityP->type != CorObject))
    return migrateFail(msP, kind, "the entity is no JSON object");

  if (migrateEntityTermsCheck(msP, kind, entityP) == false)
    return false;

  corLdExpandEntityTree(entityP, (msP->contextP != NULL) ? msP->contextP : corLdCoreContext(), &corRest.kalloc);

  int64_t createdAt  = migrateTimeTake(entityP, CorTermCreatedAt,  LD_VOCAB_CREATED_AT);
  int64_t modifiedAt = migrateTimeTake(entityP, CorTermModifiedAt, LD_VOCAB_MODIFIED_AT);

  if ((createdAt < 0) || (modifiedAt < 0))
    return migrateFail(msP, kind, "the entity's createdAt/modifiedAt is no DateTime");

  if (modifiedAt == 0) modifiedAt = (createdAt != 0) ? createdAt : (int64_t) corRest.requestStartTime;
  if (createdAt  == 0) createdAt  = modifiedAt;

  SysTimes* timesP = NULL;

  for (CorNode* attrP = entityP->value.head; attrP != NULL; attrP = attrP->next)
  {
    if ((attrP->name == NULL) || (ldIsEntityMember(attrP) == true))
      continue;

    bool ok = true;

    if (attrP->type == CorObject)
      ok = instanceTimesTake(&timesP, attrP->name, attrP);
    else if (attrP->type == CorArray)
    {
      for (CorNode* instP = attrP->value.head; (instP != NULL) && (ok == true); instP = instP->next)
        ok = instanceTimesTake(&timesP, attrP->name, instP);
    }

    if (ok == false)
      return migrateFail(msP, kind, "a createdAt/modifiedAt of attribute '%s' is no DateTime", attrP->name);
  }

  if ((ldNormalizeInput(entityP, &corRest.kalloc, false, false) == false) ||
      (ldCheckEntity(entityP, LdOpCreateEntity, NULL, &corRest.kalloc) == false))
    return migrateFail(msP, kind, "%s", migrateProblem());

  corRest.requestStartTime = (uint64_t) modifiedAt;   // what the conversion stamps where the source had nothing
  ldApiEntityToDbModel(entityP, &corRest.kalloc, createdAt);
  timesRestore(entityP, timesP);

  *createdAtP  = createdAt;
  *modifiedAtP = modifiedAt;

  return true;
}



// -----------------------------------------------------------------------------
//
// migrateEntity - import one Entity (current state)
//
bool migrateEntity(MigrateState* msP, Tenant* tenantP, CorNode* entityP)
{
  int64_t createdAt;
  int64_t modifiedAt;

  if (migrateEntityToDbModel(msP, MigrateEntity, entityP, &createdAt, &modifiedAt) == false)
    return false;

  CorNode* idP = corTreeLookup(entityP, "id");

  if ((idP == NULL) || (idP->type != CorString))
    return migrateFail(msP, MigrateEntity, "the entity has no id");

  const char* entityId = idP->value.s;
  int         r        = db.entityCreate(tenantP, entityId, entityP);

  if (r == DB_ALREADY_EXISTS)
    return migrateFail(msP, MigrateEntity, "'%s' already exists in the target", entityId);
  if (r != DB_OK)
    return migrateFail(msP, MigrateEntity, "'%s' was not stored (database error %d)", entityId, r);

  msP->okV[MigrateEntity] += 1;
  return true;
}
