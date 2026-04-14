//
// FILE            ramdbSubscriptionUpdate.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//
#include <string.h>                                   // strcmp

#include "kjson/KjNode.h"                             // KjNode
#include "kjson/kjClone.h"                            // kjClone
#include "kjson/kjBuilder.h"                          // kjChildAdd, kjChildRemove
#include "kjson/kjLookup.h"                           // kjLookup

#include "db/DbDriver.h"                              // DB_OK, DB_NOT_FOUND, Tenant
#include "currentState/swRamDB/ramdbStore.h"          // ramdbSubscriptions
#include "currentState/swRamDB/ramdbSubscriptionUpdate.h"  // Own interface



// -----------------------------------------------------------------------------
//
// ramdbSubscriptionUpdate - JSON Merge Patch semantics
//
// For each field in the fragment:
//   - if null value:  remove the field from the stored subscription
//   - otherwise:      replace (or add) the field in the stored subscription
//
int ramdbSubscriptionUpdate(Tenant* tenantP, const char* subId, KjNode* fragmentP)
{
  KjNode* subscriptions = ramdbSubscriptions(tenantP);

  //
  // Find the subscription
  //
  KjNode* subP = NULL;

  for (KjNode* sP = subscriptions->value.firstChildP; sP != NULL; sP = sP->next)
  {
    KjNode* idP = kjLookup(sP, "id");

    if (idP != NULL && idP->type == KjString && strcmp(idP->value.s, subId) == 0)
    {
      subP = sP;
      break;
    }
  }

  if (subP == NULL)
    return DB_NOT_FOUND;

  //
  // Apply merge-patch: iterate the fragment and update the stored subscription
  //
  KjNode* next;

  for (KjNode* fieldP = fragmentP->value.firstChildP; fieldP != NULL; fieldP = next)
  {
    next = fieldP->next;

    KjNode* existingP = kjLookup(subP, fieldP->name);

    if (fieldP->type == KjNull)
    {
      // Remove the field if it exists
      if (existingP != NULL)
        kjChildRemove(subP, existingP);
    }
    else
    {
      // Replace or add
      if (existingP != NULL)
        kjChildRemove(subP, existingP);

      KjNode* cloneP = kjClone(NULL, fieldP);
      kjChildAdd(subP, cloneP);
    }
  }

  return DB_OK;
}
