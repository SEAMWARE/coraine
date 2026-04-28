//
// FILE            deleteEntityTemporalAttr.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//
// DELETE /ngsi-ld/v1/temporal/entities/{id}/attrs/{attr} (§ 5.6.13 / § 6.21.3.1).
// URL params:
//   ?datasetId=<uri>   — limit deletion to one dataset
//   ?deleteAll=true    — every instance regardless of datasetId
// Defaults: only the default-dataset (datasetId='') is deleted.
//

#include <stddef.h>                                  // NULL

#include "swRest/SwRestState.h"                      // swRest
#include "swJsonld/swldInit.h"                       // swldCoreContext
#include "swJsonld/swldExpand.h"                     // swldExpand

#include "swNgsild/swNgsild.h"                       // ldError, LD_ERROR_*, swNgsild
#include "swNgsild/SwNgsild.h"                       // swNgsild fields

#include "troe/TroeDriver.h"                         // troe

#include "db/Tenant.h"                               // Tenant

#include "serviceRoutines/deleteEntityTemporalAttr.h"  // Own interface



bool deleteEntityTemporalAttr(void)
{
  const char* entityId = swRest.in.wildcard[0];
  const char* attrWild = swRest.in.wildcard[1];

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

  if (troe.entityTemporalAttrDelete == NULL)
  {
    ldError(501, "https://uri.etsi.org/ngsi-ld/errors/OperationNotSupported",
            "Not Implemented",
            "active TRoE plugin does not support attribute delete on temporal entities");
    return true;
  }

  // Expand attribute name via the request @context (or core context).
  ldContextResolve();
  SwldContext* ctxP    = (swNgsild.contextP != NULL) ? swNgsild.contextP : swldCoreContext();
  const char*  attrIri = swldExpand(ctxP, attrWild, &swRest.kalloc, NULL, NULL);
  if (attrIri == NULL) attrIri = attrWild;

  Tenant* tenantP = (Tenant*) swNgsild.tenantP;

  // datasetId: NULL → default dataset; deleteAll bool from URL parser.
  const char* datasetId = NULL;
  if (swNgsild.datasetIdV != NULL && swNgsild.datasetIdV[0] != NULL)
    datasetId = swNgsild.datasetIdV[0];

  int r = troe.entityTemporalAttrDelete(tenantP, entityId, attrIri,
                                        datasetId, swNgsild.deleteAll);

  if (r == TROE_NOT_FOUND)
  {
    ldError(404, LD_ERROR_RESOURCE_NOT_FOUND, "Not Found",
            "no temporal data for entity '%s' / attribute '%s'", entityId, attrWild);
    return true;
  }
  if (r != TROE_OK)
  {
    ldError(500, LD_ERROR_INTERNAL_ERROR, "Internal Error",
            "temporal attribute delete failed for '%s'/'%s'", entityId, attrWild);
    return true;
  }

  swRest.out.httpStatusCode = 204;
  return true;
}
