//
// FILE            getEntityAttr.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//
// GET /ngsi-ld/v1/entities/{entityId}/attrs/{attrId}
//
// Retrieve a single NGSI-LD Attribute. Not in v1.9.1; pre-emptive addition
// awaiting the next spec release. See doc/spec-doubts.md entry #13.
//
// Local-only for now — distops forwarding for this endpoint is a follow-up
// once the operation name is standardized (candidates: "retrieveAttribute"
// vs reusing the entity retrieve flow with attrs constraint).
//

#include <stddef.h>                                  // NULL
#include <string.h>                                  // strcmp

#include "swRest/SwRestState.h"                      // swRest

#include "kjson/KjNode.h"                            // KjNode
#include "kjson/kjBuilder.h"                         // kjObject, kjChildAdd, kjChildRemove
#include "kjson/kjLookup.h"                          // kjLookup

#include "swJsonld/swldExpand.h"                     // swldExpand
#include "swJsonld/swldInit.h"                       // swldCoreContext

#include "swNgsild/swNgsild.h"                       // ldError, LD_ERROR_*, swNgsild, ldContextResolve
#include "swNgsild/ldEntityToApi.h"                  // ldEntityToApi
#include "swNgsild/ldStripSysAttrs.h"                // ldStripSysAttrs

#include "db/DbDriver.h"                             // db, DB_OK, DB_NOT_FOUND
#include "db/Tenant.h"                               // Tenant

#include "serviceRoutines/getEntityAttr.h"           // Own interface



// -----------------------------------------------------------------------------
//
// getEntityAttr -
//
bool getEntityAttr(void)
{
  if (swNgsild.contextError)
    return true;

  const char* entityId = swRest.in.wildcard[0];
  const char* attrWild = swRest.in.wildcard[1];

  ldContextResolve();

  SwldContext* ctxP    = (swNgsild.contextP != NULL) ? swNgsild.contextP : swldCoreContext();
  const char*  attrIri = swldExpand(ctxP, attrWild, &swRest.kalloc, NULL, NULL);
  if (attrIri == NULL)
    attrIri = attrWild;

  Tenant* tenantP = (Tenant*) swNgsild.tenantP;

  KjNode* entityP = NULL;
  int     r       = db.entityRetrieve(tenantP, entityId, &entityP);

  if (r == DB_NOT_FOUND || entityP == NULL)
  {
    ldError(404, LD_ERROR_RESOURCE_NOT_FOUND, "Not Found", "entity '%s' not found", entityId);
    return true;
  }

  if (r != DB_OK)
  {
    ldError(500, LD_ERROR_INTERNAL_ERROR, "Internal Error",
            "database error retrieving entity '%s'", entityId);
    return true;
  }

  KjNode* attrWrapperP = kjLookup(entityP, attrIri);
  if (attrWrapperP == NULL || attrWrapperP->type != KjObject)
  {
    ldError(404, LD_ERROR_RESOURCE_NOT_FOUND, "Not Found",
            "attribute '%s' not found in entity '%s'", attrWild, entityId);
    return true;
  }

  //
  // datasetId filter — storage format keys each instance by dsKey (or
  // "@none" for the default). Remove instances not matching.
  //
  if (swNgsild.datasetIdV != NULL)
  {
    KjNode* instP = attrWrapperP->value.firstChildP;
    while (instP != NULL)
    {
      KjNode* nextP = instP->next;
      bool    keep  = false;
      for (int i = 0; swNgsild.datasetIdV[i] != NULL; i++)
      {
        if (instP->name != NULL && strcmp(instP->name, swNgsild.datasetIdV[i]) == 0)
        {
          keep = true;
          break;
        }
      }
      if (!keep)
        kjChildRemove(attrWrapperP, instP);
      instP = nextP;
    }
    if (attrWrapperP->value.firstChildP == NULL)
    {
      ldError(404, LD_ERROR_RESOURCE_NOT_FOUND, "Not Found",
              "no matching datasetId for attribute '%s' in entity '%s'", attrWild, entityId);
      return true;
    }
  }

  //
  // Unwrap storage-format to API-format via a transient entity wrapper
  // holding only the one attribute. ldEntityToApi handles the
  // single-instance/array distinction and the timestamp-to-ISO conversion.
  //
  kjChildRemove(entityP, attrWrapperP);
  KjNode* wrap = kjObject(swRest.kjsonP, NULL);
  kjChildAdd(wrap, attrWrapperP);

  ldEntityToApi(wrap, &swRest.kalloc);

  KjNode* unwrapped = wrap->value.firstChildP;
  if (unwrapped == NULL)
  {
    ldError(500, LD_ERROR_INTERNAL_ERROR, "Internal Error",
            "failed to transform attribute '%s'", attrWild);
    return true;
  }

  if (swNgsild.sysAttrs == false)
    ldStripSysAttrs(unwrapped);

  //
  // Root: rawResponse prevents renderHook from running ldEntityToApi again
  // (would misinterpret sub-attribute objects as entity-level wrappers).
  // Compaction and @context injection still run.
  //
  swRest.out.responseTree   = unwrapped;
  swRest.out.httpStatusCode = 200;
  swNgsild.rawResponse      = true;
  return true;
}
