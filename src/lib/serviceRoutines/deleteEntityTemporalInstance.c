//
// FILE            deleteEntityTemporalInstance.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//
// DELETE /ngsi-ld/v1/temporal/entities/{id}/attrs/{attr}/{instance}
// (§ 5.6.15 / § 6.22.3.2). Drops a single TRoE row identified by
// instanceId.
//

#include <stddef.h>                                  // NULL

#include "swRest/SwRestState.h"                      // swRest
#include "swJsonld/swldInit.h"                       // swldCoreContext
#include "swJsonld/swldExpand.h"                     // swldExpand

#include "swNgsild/swNgsild.h"                       // ldError, LD_ERROR_*, swNgsild

#include "troe/TroeDriver.h"                         // troe

#include "db/Tenant.h"                               // Tenant

#include "serviceRoutines/deleteEntityTemporalInstance.h"  // Own interface



bool deleteEntityTemporalInstance(void)
{
  const char* entityId   = swRest.in.wildcard[0];
  const char* attrWild   = swRest.in.wildcard[1];
  const char* instanceId = swRest.in.wildcard[2];

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

  if (troe.entityTemporalInstanceDelete == NULL)
  {
    ldError(501, "https://uri.etsi.org/ngsi-ld/errors/OperationNotSupported",
            "Not Implemented",
            "active TRoE plugin does not support instance delete");
    return true;
  }

  ldContextResolve();
  SwldContext* ctxP    = (swNgsild.contextP != NULL) ? swNgsild.contextP : swldCoreContext();
  const char*  attrIri = swldExpand(ctxP, attrWild, &swRest.kalloc, NULL, NULL);
  if (attrIri == NULL) attrIri = attrWild;

  Tenant* tenantP = (Tenant*) swNgsild.tenantP;

  int r = troe.entityTemporalInstanceDelete(tenantP, entityId, attrIri, instanceId);

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
            "temporal instance delete failed");
    return true;
  }

  swRest.out.httpStatusCode = 204;
  return true;
}
