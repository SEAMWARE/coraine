//
// FILE            corDbSubscriptionUpdate.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
#include <string.h>                                   // strcmp

#include "corTree/CorNode.h"                          // CorNode
#include "corTree/corTreeClone.h"                     // corTreeClone
#include "corTree/corTreeBuilder.h"                   // corTreeChildAdd, corTreeChildRemove
#include "corTree/corTreeLookup.h"                    // corTreeLookup

#include "corTree/corTreeFree.h"                      // corTreeFree
#include "db/DbDriver.h"                              // DB_OK, DB_NOT_FOUND, Tenant
#include "currentState/corDB/corDbStore.h"          // corDbSubscriptions
#include "currentState/corDB/corDbSubscriptionUpdate.h"  // Own interface



// -----------------------------------------------------------------------------
//
// corDbSubscriptionUpdate - JSON Merge Patch semantics
//
// For each field in the fragment:
//   - if null value:  remove the field from the stored subscription
//   - otherwise:      replace (or add) the field in the stored subscription
//
int corDbSubscriptionUpdate(Tenant* tenantP, const char* subId, CorNode* fragmentP)
{
  COR_DB_WRITE(tenantP);

  CorNode* subscriptions = corDbSubscriptions(tenantP);

  //
  // Find the subscription
  //
  CorNode* subP = NULL;

  for (CorNode* sP = subscriptions->value.head; sP != NULL; sP = sP->next)
  {
    CorNode* idP = corTreeLookup(sP, "id");

    if (idP != NULL && idP->type == CorString && strcmp(idP->value.s, subId) == 0)
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
  CorNode* next;

  for (CorNode* fieldP = fragmentP->value.head; fieldP != NULL; fieldP = next)
  {
    next = fieldP->next;

    CorNode* existingP = corTreeLookup(subP, fieldP->name);

    if (fieldP->type == CorNull)
    {
      // Remove the field if it exists
      if (existingP != NULL)
      {
        corTreeChildRemove(subP, existingP);
        corTreeFree(existingP);
      }
    }
    else
    {
      // Replace or add
      if (existingP != NULL)
      {
        corTreeChildRemove(subP, existingP);
        corTreeFree(existingP);
      }

      CorNode* cloneP = corTreeClone(NULL, fieldP);
      corTreeChildAdd(subP, cloneP);
    }
  }

  return DB_OK;
}
