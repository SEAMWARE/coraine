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

#include "corLog/corLog.h"                            // COR_E
#include "corTree/CorNode.h"                          // CorNode
#include "corTree/corTreeClone.h"                     // corTreeClone
#include "corTree/corTreeBuilder.h"                   // corTreeChildAdd, corTreeChildRemove
#include "corTree/corTreeLookup.h"                    // corTreeLookup

#include "corTree/corTreeFree.h"                      // corTreeFree
#include "corTree/corTreeChildReplace.h"              // corTreeChildReplace
#include "db/DbDriver.h"                              // DB_OK, DB_NOT_FOUND, DB_ERR, Tenant
#include "currentState/corDB/corDbStore.h"          // corDbRegistrations
#include "currentState/corDB/corDbRegistrationUpdate.h"  // Own interface



// -----------------------------------------------------------------------------
//
// corDbRegistrationUpdate - replace the stored registration with `regP`
//
int corDbRegistrationUpdate(Tenant* tenantP, const char* regId, CorNode* regP)
{
  COR_DB_WRITE(tenantP);

  CorNode* registrations = corDbRegistrations(tenantP);

  for (CorNode* rP = registrations->value.head; rP != NULL; rP = rP->next)
  {
    CorNode* idP = corTreeLookup(rP, "id");

    if (idP != NULL && idP->type == CorString && strcmp(idP->value.s, regId) == 0)
    {
      CorNode* cloneP = corTreeClone(NULL, regP);
      if (cloneP == NULL)
      {
        COR_E("corDB: corTreeClone failed for registration '%s'", regId);
        return DB_ERR;
      }

      corTreeChildReplace(registrations, rP, cloneP);
      corTreeFree(rP);
      return DB_OK;
    }
  }

  return DB_NOT_FOUND;
}
