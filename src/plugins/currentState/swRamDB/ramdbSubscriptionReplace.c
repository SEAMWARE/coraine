//
// FILE            ramdbSubscriptionReplace.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//
// Stores a full subscription document, replacing whatever is on record for
// `subId`. The NGSI-LD merge (JSON Merge Patch incl. the urn:ngsi-ld:null
// delete-marker) is resolved by the broker before this is called, so the DB
// plugin is a dumb store — it never interprets NGSI-LD null semantics.
//
#include <string.h>                                   // strcmp

#include "ktrace/kTrace.h"                            // KT_E
#include "kjson/KjNode.h"                             // KjNode
#include "kjson/kjClone.h"                            // kjClone
#include "kjson/kjBuilder.h"                          // kjChildAdd, kjChildRemove
#include "kjson/kjLookup.h"                           // kjLookup

#include "kjson/kjFree.h"                             // kjFree
#include "kjson/kjChildReplace.h"                     // kjChildReplace
#include "db/DbDriver.h"                              // DB_OK, DB_NOT_FOUND, DB_ERR, Tenant
#include "currentState/swRamDB/ramdbStore.h"          // ramdbSubscriptions
#include "currentState/swRamDB/ramdbSubscriptionReplace.h"  // Own interface



// -----------------------------------------------------------------------------
//
// ramdbSubscriptionReplace - replace the stored subscription with `subP`
//
int ramdbSubscriptionReplace(Tenant* tenantP, const char* subId, KjNode* subP)
{
  KjNode* subscriptions = ramdbSubscriptions(tenantP);

  for (KjNode* sP = subscriptions->value.firstChildP; sP != NULL; sP = sP->next)
  {
    KjNode* idP = kjLookup(sP, "id");

    if (idP != NULL && idP->type == KjString && strcmp(idP->value.s, subId) == 0)
    {
      KjNode* cloneP = kjClone(NULL, subP);
      if (cloneP == NULL)
      {
        KT_E("swRamDB: kjClone failed for subscription '%s'", subId);
        return DB_ERR;
      }

      kjChildReplace(subscriptions, sP, cloneP);
      kjFree(sP);
      return DB_OK;
    }
  }

  return DB_NOT_FOUND;
}
