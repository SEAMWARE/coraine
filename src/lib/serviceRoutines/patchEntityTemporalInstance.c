//
// FILE            patchEntityTemporalInstance.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//
// PATCH /ngsi-ld/v1/temporal/entities/{id}/attrs/{attr}/{instance}
// (§ 5.6.14 / § 6.22.3.1) — Modify a single Attribute instance, identified
// by the broker-generated instanceId, in TRoE.
//
// Body: EntityTemporal Fragment containing exactly one new instance — the
// plugin extracts its value-bearing fields and overwrites the existing
// row's value/observed_at columns. modified_at is left intact (PK).
//

#include <stddef.h>                                  // NULL

#include "swRest/SwRestState.h"                      // swRest
#include "swJsonld/swldInit.h"                       // swldCoreContext
#include "swJsonld/swldExpand.h"                     // swldExpand

#include "kjson/KjNode.h"                            // KjNode

#include "swNgsild/swNgsild.h"                       // ldError, LD_ERROR_*, swNgsild

#include "troe/TroeDriver.h"                         // troe

#include "db/Tenant.h"                               // Tenant

#include "serviceRoutines/patchEntityTemporalInstance.h"  // Own interface



bool patchEntityTemporalInstance(void)
{
  const char* entityId   = swRest.in.wildcard[0];
  const char* attrWild   = swRest.in.wildcard[1];
  const char* instanceId = swRest.in.wildcard[2];
  KjNode*     bodyP      = swRest.in.requestTree;

  if (entityId == NULL || entityId[0] == 0)
  {
    ldError(400, LD_ERROR_BAD_REQUEST_DATA, "Bad Request", "missing entity id in URL");
    return true;
  }
  if (attrWild == NULL || attrWild[0] == 0)
  {
    ldError(400, LD_ERROR_BAD_REQUEST_DATA, "Bad Request", "missing attribute name in URL");
    return true;
  }
  if (instanceId == NULL || instanceId[0] == 0)
  {
    ldError(400, LD_ERROR_BAD_REQUEST_DATA, "Bad Request", "missing instance id in URL");
    return true;
  }
  if (bodyP == NULL || bodyP->type != KjObject)
  {
    ldError(400, LD_ERROR_BAD_REQUEST_DATA, "Bad Request",
            "request body must be a JSON-LD object (EntityTemporal Fragment)");
    return true;
  }

  if (troe.entityTemporalInstanceModify == NULL)
  {
    ldError(501, "https://uri.etsi.org/ngsi-ld/errors/OperationNotSupported",
            "Not Implemented",
            "active TRoE plugin does not support instance modify");
    return true;
  }

  // Expand attribute name via the request @context (or core context).
  ldContextResolve();
  SwldContext* ctxP    = (swNgsild.contextP != NULL) ? swNgsild.contextP : swldCoreContext();
  const char*  attrIri = swldExpand(ctxP, attrWild, &swRest.kalloc, NULL, NULL);
  if (attrIri == NULL) attrIri = attrWild;

  Tenant* tenantP = (Tenant*) swNgsild.tenantP;

  int r = troe.entityTemporalInstanceModify(tenantP, entityId, attrIri, instanceId, bodyP);

  if (r == TROE_NOT_FOUND)
  {
    ldError(404, LD_ERROR_RESOURCE_NOT_FOUND, "Not Found",
            "no temporal instance '%s' for entity '%s' / attribute '%s'",
            instanceId, entityId, attrWild);
    return true;
  }
  if (r != TROE_OK)
  {
    ldError(500, LD_ERROR_INTERNAL_ERROR, "Internal Error",
            "temporal instance modify failed");
    return true;
  }

  swRest.out.httpStatusCode = 204;
  return true;
}
