//
// FILE            corDbRegistrationUpdate.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
// Stores a full registration document, replacing whatever is on record for
// `regId`. The NGSI-LD merge (JSON Merge Patch incl. the urn:ngsi-ld:null
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
#include "currentState/corDB/corDbStore.h"          // corDbRegistrations
#include "currentState/corDB/corDbRegistrationUpdate.h"  // Own interface



// -----------------------------------------------------------------------------
//
// corDbRegistrationUpdate - replace the stored registration with `regP`
//
int corDbRegistrationUpdate(Tenant* tenantP, const char* regId, KjNode* regP)
{
  KjNode* registrations = corDbRegistrations(tenantP);

  for (KjNode* rP = registrations->value.firstChildP; rP != NULL; rP = rP->next)
  {
    KjNode* idP = kjLookup(rP, "id");

    if (idP != NULL && idP->type == KjString && strcmp(idP->value.s, regId) == 0)
    {
      KjNode* cloneP = kjClone(NULL, regP);
      if (cloneP == NULL)
      {
        KT_E("corDB: kjClone failed for registration '%s'", regId);
        return DB_ERR;
      }

      kjChildReplace(registrations, rP, cloneP);
      kjFree(rP);
      return DB_OK;
    }
  }

  return DB_NOT_FOUND;
}
