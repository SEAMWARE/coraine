//
// FILE            troeFromMerge.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//

#include <stddef.h>                                   // NULL
#include <string.h>                                   // strcmp, memset

#include "kalloc/kaAlloc.h"                          // kaAlloc
#include "kjson/KjNode.h"                             // KjNode
#include "kjson/kjLookup.h"                           // kjLookup

#include "swRest/SwRestState.h"                       // swRest

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

    TroeEvent* tevP = (TroeEvent*) kaAlloc(&swRest.kalloc, sizeof(TroeEvent));
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
