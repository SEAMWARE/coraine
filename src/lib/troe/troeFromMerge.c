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

#include "kalloc/kaAlloc.h"                          // kaAlloc
#include "kjson/KjNode.h"                             // KjNode
#include "kjson/kjLookup.h"                           // kjLookup
#include "kjson/kjBuilder.h"                          // kjChildAdd
#include "kjson/kjClone.h"                            // kjClone

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
static bool multiInstance(KjNode* attrP)
{
  return (attrP != NULL) && (attrP->type == KjObject) &&
         (attrP->value.firstChildP != NULL) && (attrP->value.firstChildP->next != NULL);
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
                          KjNode* mergedEntity, KjNode* attrP, KjNode* instP, uint64_t modifiedAtNs)
{
  KjNode* wrapperP = (KjNode*) kaAlloc(&corRest.kalloc, sizeof(KjNode));

  *wrapperP = *attrP;
  wrapperP->next              = NULL;
  wrapperP->value.firstChildP = NULL;
  wrapperP->lastChild         = NULL;
  kjChildAdd(wrapperP, kjClone(corRest.kjsonP, instP));

  TroeEvent* tevP = (TroeEvent*) kaAlloc(&corRest.kalloc, sizeof(TroeEvent));
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
// instanceEvents - an event per instance the write touched
//
// ldInstanceWritten is the one answer to "touched" - the one a subscription
// watching attr@datasetId gets too. An instance only in preP went away.
//
static void instanceEvents(Tenant* tenantP, const char* entityId, const char* entityType, const char* attrName,
                           KjNode* mergedEntity, KjNode* preP, KjNode* postP, uint64_t modifiedAtNs)
{
  if ((postP != NULL) && (postP->type == KjObject))
  {
    for (KjNode* instP = postP->value.firstChildP; instP != NULL; instP = instP->next)
    {
      if ((instP->name == NULL) || (instP->type != KjObject) || (ldInstanceWritten(preP, postP, instP->name) == false))
        continue;

      bool   isNew = (preP == NULL) || (kjLookup(preP, instP->name) == NULL);
      TroeOp op    = (isNew == true) ? TroeOpAttrCreated : TroeOpAttrModified;

      instanceEvent(op, tenantP, entityId, entityType, attrName, mergedEntity, postP, instP, modifiedAtNs);
    }
  }

  if ((preP != NULL) && (preP->type == KjObject))
  {
    for (KjNode* instP = preP->value.firstChildP; instP != NULL; instP = instP->next)
    {
      if ((instP->name == NULL) || (instP->type != KjObject))
        continue;

      if ((postP == NULL) || (kjLookup(postP, instP->name) == NULL))
        instanceEvent(TroeOpAttrDeleted, tenantP, entityId, entityType, attrName, mergedEntity, preP, instP, modifiedAtNs);
    }
  }
}



// -----------------------------------------------------------------------------
//
// troeDeferAttrEventsFromMerge -
//
void troeDeferAttrEventsFromMerge(Tenant*         tenantP,
                                  const char*     entityId,
                                  const char*     entityType,
                                  KjNode*         mergedEntity,
                                  LdMergeReport*  reportP,
                                  uint64_t        modifiedAtNs)
{
  if (reportP == NULL || reportP->changes == NULL)
    return;

  for (KjNode* changeP = reportP->changes->value.firstChildP; changeP != NULL; changeP = changeP->next)
  {
    KjNode* attrP   = kjLookup(changeP, "attr");
    KjNode* reasonP = kjLookup(changeP, "reason");

    const char* attrName = (attrP   != NULL && attrP->type   == KjString) ? attrP->value.s   : NULL;
    const char* reason   = (reasonP != NULL && reasonP->type == KjString) ? reasonP->value.s : NULL;

    if (attrName == NULL)
      continue;

    KjNode* attrSnapshot = NULL;
    if (mergedEntity != NULL)
      attrSnapshot = kjLookup(mergedEntity, attrName);

    // A deleted attr is gone from mergedEntity; the report's preValue clone
    // (the pre-delete wrapper) still knows the attr kind — needed for the
    // tombstone row's attr_kind (§ 5.3.2.5: a deleted instance keeps the
    // Attribute's type).
    if (attrSnapshot == NULL)
      attrSnapshot = kjLookup(changeP, "preValue");

    //
    // An Attribute with several instances: a row for each instance the write
    // touched, and for no other. The snapshot is the whole Attribute - every
    // instance - so a single event would record them all (or, read by its
    // first instance, the wrong one).
    //
    KjNode* preP  = kjLookup(changeP, "preValue");
    KjNode* postP = (mergedEntity != NULL) ? kjLookup(mergedEntity, attrName) : NULL;

    if (multiInstance(preP) || multiInstance(postP))
    {
      instanceEvents(tenantP, entityId, entityType, attrName, mergedEntity, preP, postP, modifiedAtNs);
      continue;
    }

    TroeEvent* tevP = (TroeEvent*) kaAlloc(&corRest.kalloc, sizeof(TroeEvent));
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
