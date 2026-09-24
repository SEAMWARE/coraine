//
// FILE            bridgeAttrsOut.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//

#include <stdbool.h>                                  // bool
#include <stddef.h>                                   // NULL
#include <string.h>                                   // strcmp

#include "kjson/KjNode.h"                             // KjNode
#include "kjson/kjLookup.h"                           // kjLookup
#include "kjson/kjBuilder.h"                          // kjArray, kjChildAdd, kjChildRemove
#include "kjson/kjClone.h"                            // kjClone
#include "ktrace/kTrace.h"                            // KT_T

#include "corBridge/BridgeDriver.h"                   // bridgeCount
#include "corNgsild/ldIsEntityKeyword.h"              // ldIsNotAttributeName

#include "bridge/channelCache.h"                      // channelCount
#include "bridge/bridgeAttrOut.h"                     // bridgeAttrOut
#include "bridge/bridgeAttrsOut.h"                    // Own interface
#include "coraineTraceLevels.h"                       // KtBridge



// -----------------------------------------------------------------------------
//
// nothingToDo - the common case, and it must cost nothing
//
// Two integer loads before any tree is walked. Every deployment that does not
// use bridges takes this branch on every write, so the walk itself has to be
// on the other side of it.
//
static bool nothingToDo(const char* entityId)
{
  if ((bridgeCount == 0) || (channelCount() == 0))
    return true;

  return (entityId == NULL);
}



// -----------------------------------------------------------------------------
//
// bridgeAttrsOutFromMerge -
//
void bridgeAttrsOutFromMerge(Tenant* tenantP, const char* entityId, KjNode* entityP, LdMergeReport* reportP, const BridgeSyncDone* syncDoneP)
{
  if (nothingToDo(entityId) == true)
    return;

  if ((reportP == NULL) || (reportP->changes == NULL))
    return;

  for (KjNode* changeP = reportP->changes->value.firstChildP; changeP != NULL; changeP = changeP->next)
  {
    KjNode* attrP   = kjLookup(changeP, "attr");
    KjNode* reasonP = kjLookup(changeP, "reason");

    if ((attrP == NULL) || (attrP->type != KjString))
      continue;

    //
    // A deletion is not a value, and a transport has no way to carry one. See
    // the header - this is the decision, not an oversight.
    //
    if ((reasonP != NULL) && (reasonP->type == KjString) && (strcmp(reasonP->value.s, "attributeDeleted") == 0))
    {
      KT_T(KtBridge, "%s/%s was deleted - nothing to publish", entityId, attrP->value.s);
      continue;
    }

    bridgeAttrOut(tenantP, entityId, attrP->value.s, entityP, syncDoneP);
  }
}



// -----------------------------------------------------------------------------
//
// bridgeChangesAccumulate -
//
void bridgeChangesAccumulate(LdMergeReport* accP, LdMergeReport* reportP, Kjson* kjsonP)
{
  if ((bridgeCount == 0) || (channelCount() == 0))
    return;

  if ((reportP == NULL) || (reportP->changes == NULL))
    return;

  if (accP->changes == NULL)
    accP->changes = kjArray(kjsonP, NULL);

  for (KjNode* changeP = reportP->changes->value.firstChildP; changeP != NULL; changeP = changeP->next)
  {
    KjNode* attrP = kjLookup(changeP, "attr");

    if ((attrP == NULL) || (attrP->type != KjString))
      continue;

    for (KjNode* earlierP = accP->changes->value.firstChildP; earlierP != NULL; earlierP = earlierP->next)
    {
      KjNode* earlierAttrP = kjLookup(earlierP, "attr");

      if ((earlierAttrP != NULL) && (earlierAttrP->type == KjString) && (strcmp(earlierAttrP->value.s, attrP->value.s) == 0))
      {
        kjChildRemove(accP->changes, earlierP);
        break;
      }
    }

    // A clone: the report is still the notification's and the TRoE events'
    kjChildAdd(accP->changes, kjClone(kjsonP, changeP));
  }
}



// -----------------------------------------------------------------------------
//
// bridgeAttrsOutFromEntity -
//
void bridgeAttrsOutFromEntity(Tenant* tenantP, const char* entityId, KjNode* entityP)
{
  if (nothingToDo(entityId) == true)
    return;

  if ((entityP == NULL) || (entityP->type != KjObject))
    return;

  for (KjNode* attrP = entityP->value.firstChildP; attrP != NULL; attrP = attrP->next)
  {
    if (ldIsNotAttributeName(attrP->name) == true)
      continue;

    //
    // The DB model's own members - "_id", and whatever else a driver keeps
    // beside the attributes. They are not @-prefixed and not entity keywords,
    // so the name-based filter above lets them through; the Channel lookup
    // then refuses them, as it refuses any name no configuration named.
    //
    bridgeAttrOut(tenantP, entityId, attrP->name, entityP, NULL);
  }
}
