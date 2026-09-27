//
// FILE            troeFromMerge.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//

#include <stdbool.h>                                  // bool
#include <stddef.h>                                   // NULL
#include <string.h>                                   // strcmp, memset

#include "corAlloc/corAlloc.h"                       // corAlloc
#include "corTree/CorNode.h"                          // CorNode
#include "corTree/corTreeLookup.h"                    // corTreeLookup
#include "corTree/corTreeBuilder.h"                   // corTreeChildAdd
#include "corTree/corTreeClone.h"                     // corTreeClone

#include "corRest/CorRestState.h"                       // corRest
#include "corNgsild/ldInstanceWritten.h"              // ldInstanceWritten

#include "troe/TroeDriver.h"                          // TroeEvent, TroeOp*
#include "troe/troeDispatch.h"                        // troeDeferAttrEvent
#include "troe/troeFromMerge.h"                       // Own interface



// -----------------------------------------------------------------------------
//
// reasonToOp - map LdMergeReport reason string to TroeOp.
//
static TroeOp reasonToOp(const char* reason)
{
  if (reason == NULL)                              return TroeOpAttrModified;
  if (strcmp(reason, "attributeCreated")  == 0)    return TroeOpAttrCreated;
  if (strcmp(reason, "attributeDeleted")  == 0)    return TroeOpAttrDeleted;
  /* "attributeModified" or anything else */       return TroeOpAttrModified;
}



// -----------------------------------------------------------------------------
//
// multiInstance - is this dataset-keyed Attribute more than one instance?
//
static bool multiInstance(CorNode* attrP)
{
  return (attrP != NULL) && (attrP->type == CorObject) &&
         (attrP->value.head != NULL) && (attrP->value.head->next != NULL);
}



// -----------------------------------------------------------------------------
//
// instanceEvent - defer the event of ONE instance, in a one-instance wrapper
//
// The wrapper is a copy of the Attribute's with a clone of the instance as its
// only member: the deferred event outlives nothing, but it must not share the
// instance's ->next with the entity.
//
static void instanceEvent(TroeOp op, Tenant* tenantP, const char* entityId, const char* entityType, const char* attrName,
                          CorNode* mergedEntity, CorNode* attrP, CorNode* instP, uint64_t modifiedAtNs)
{
  CorNode* wrapperP = (CorNode*) corAlloc(&corRest.kalloc, sizeof(CorNode));

  *wrapperP = *attrP;
  wrapperP->next              = NULL;
  wrapperP->value.head = NULL;
  wrapperP->value.tail        = NULL;
  corTreeChildAdd(wrapperP, corTreeClone(corRest.kallocP, instP));

  TroeEvent* tevP = (TroeEvent*) corAlloc(&corRest.kalloc, sizeof(TroeEvent));
  memset(tevP, 0, sizeof(*tevP));
  tevP->op             = op;
  tevP->tenantP        = tenantP;
  tevP->entityId       = entityId;
  tevP->entityType     = entityType;
  tevP->attrName       = attrName;
  tevP->datasetId      = (strcmp(instP->name, "@none") != 0) ? instP->name : "";
  tevP->modifiedAtNs   = modifiedAtNs;
  tevP->attrSnapshot   = wrapperP;
  tevP->entitySnapshot = mergedEntity;
  troeDeferAttrEvent(tevP);
}



// -----------------------------------------------------------------------------
//
// removedInstances - a deletion event for each instance of preP that postP lacks
//
static void removedInstances(Tenant* tenantP, const char* entityId, const char* entityType, const char* attrName,
                             CorNode* entityP, CorNode* preP, CorNode* postP, uint64_t modifiedAtNs)
{
  if ((preP == NULL) || (preP->type != CorObject))
    return;

  for (CorNode* instP = preP->value.head; instP != NULL; instP = instP->next)
  {
    if ((instP->name == NULL) || (instP->type != CorObject))
      continue;

    if ((postP == NULL) || (corTreeLookup(postP, instP->name) == NULL))
      instanceEvent(TroeOpAttrDeleted, tenantP, entityId, entityType, attrName, entityP, preP, instP, modifiedAtNs);
  }
}



// -----------------------------------------------------------------------------
//
// instanceEvents - an event per instance the write touched
//
// ldInstanceWritten is the one answer to "touched" - the one a subscription
// watching attr@datasetId gets too. An instance only in preP went away.
//
static void instanceEvents(Tenant* tenantP, const char* entityId, const char* entityType, const char* attrName,
                           CorNode* mergedEntity, CorNode* preP, CorNode* postP, uint64_t modifiedAtNs)
{
  if ((postP != NULL) && (postP->type == CorObject))
  {
    for (CorNode* instP = postP->value.head; instP != NULL; instP = instP->next)
    {
      if ((instP->name == NULL) || (instP->type != CorObject) || (ldInstanceWritten(preP, postP, instP->name) == false))
        continue;

      bool   isNew = (preP == NULL) || (corTreeLookup(preP, instP->name) == NULL);
      TroeOp op    = (isNew == true) ? TroeOpAttrCreated : TroeOpAttrModified;

      instanceEvent(op, tenantP, entityId, entityType, attrName, mergedEntity, postP, instP, modifiedAtNs);
    }
  }

  removedInstances(tenantP, entityId, entityType, attrName, mergedEntity, preP, postP, modifiedAtNs);
}



// -----------------------------------------------------------------------------
//
// troeDeferAttrEventsFromMerge -
//
void troeDeferAttrEventsFromMerge(Tenant*         tenantP,
                                  const char*     entityId,
                                  const char*     entityType,
                                  CorNode*        mergedEntity,
                                  LdMergeReport*  reportP,
                                  uint64_t        modifiedAtNs)
{
  if (reportP == NULL || reportP->changes == NULL)
    return;

  for (CorNode* changeP = reportP->changes->value.head; changeP != NULL; changeP = changeP->next)
  {
    CorNode* attrP  = corTreeLookup(changeP, "attr");
    CorNode* reasonP = corTreeLookup(changeP, "reason");

    const char* attrName = (attrP   != NULL && attrP->type   == CorString) ? attrP->value.s  : NULL;
    const char* reason   = (reasonP != NULL && reasonP->type == CorString) ? reasonP->value.s : NULL;

    if (attrName == NULL)
      continue;

    CorNode* attrSnapshot = NULL;
    if (mergedEntity != NULL)
      attrSnapshot = corTreeLookup(mergedEntity, attrName);

    // A deleted attr is gone from mergedEntity; the report's preValue clone
    // (the pre-delete wrapper) still knows the attr kind — needed for the
    // tombstone row's attr_kind (§ 5.3.2.5: a deleted instance keeps the
    // Attribute's type).
    if (attrSnapshot == NULL)
      attrSnapshot = corTreeLookup(changeP, "preValue");

    //
    // An Attribute with several instances: a row for each instance the write
    // touched, and for no other. The snapshot is the whole Attribute - every
    // instance - so a single event would record them all (or, read by its
    // first instance, the wrong one).
    //
    CorNode* preP = corTreeLookup(changeP, "preValue");
    CorNode* postP = (mergedEntity != NULL) ? corTreeLookup(mergedEntity, attrName) : NULL;

    if (multiInstance(preP) || multiInstance(postP))
    {
      instanceEvents(tenantP, entityId, entityType, attrName, mergedEntity, preP, postP, modifiedAtNs);
      continue;
    }

    TroeEvent* tevP = (TroeEvent*) corAlloc(&corRest.kalloc, sizeof(TroeEvent));
    memset(tevP, 0, sizeof(*tevP));
    tevP->op             = reasonToOp(reason);
    tevP->tenantP        = tenantP;
    tevP->entityId       = entityId;
    tevP->entityType     = entityType;
    tevP->attrName       = attrName;
    tevP->modifiedAtNs   = modifiedAtNs;
    tevP->attrSnapshot   = attrSnapshot;
    tevP->entitySnapshot = mergedEntity;
    troeDeferAttrEvent(tevP);
  }
}



// -----------------------------------------------------------------------------
//
// troeDeferRemovedByReplace -
//
void troeDeferRemovedByReplace(Tenant* tenantP, const char* entityId, const char* entityType, CorNode* oldEntity, CorNode* newEntity, uint64_t modifiedAtNs)
{
  if ((oldEntity == NULL) || (oldEntity->type != CorObject))
    return;

  for (CorNode* oldAttrP = oldEntity->value.head; oldAttrP != NULL; oldAttrP = oldAttrP->next)
  {
    if ((oldAttrP->name == NULL) || (oldAttrP->name[0] == '@') || (oldAttrP->type != CorObject)) continue;
    if (strcmp(oldAttrP->name, "id")         == 0)    continue;
    if (strcmp(oldAttrP->name, "_id")        == 0)    continue;
    if (strcmp(oldAttrP->name, "type")       == 0)    continue;
    if (strcmp(oldAttrP->name, "scope")      == 0)    continue;
    if (strcmp(oldAttrP->name, "createdAt")  == 0)    continue;
    if (strcmp(oldAttrP->name, "modifiedAt") == 0)    continue;

    CorNode* newAttrP = (newEntity != NULL) ? corTreeLookup(newEntity, oldAttrP->name) : NULL;

    removedInstances(tenantP, entityId, entityType, oldAttrP->name, newEntity, oldAttrP, newAttrP, modifiedAtNs);
  }
}
